/*
 * sim_engine.c  —  Deterministic waveform generator
 *
 * All waveforms are computed from (tick × tick_period_ms), giving
 * a reproducible sequence independent of wall-clock jitter.
 * No rand() — determinism means the same tick always yields the same value.
 *
 * VIVA NOTE — Why deterministic simulation?
 *   In medical device development, sensor simulation must be repeatable so
 *   that test suites can compare exact output values.  A random noise model
 *   would break regression testing.  Here we use a low-amplitude sinusoidal
 *   noise term whose period is co-prime to the breath cycle so it appears
 *   "noisy" yet is 100% reproducible.
 */

#include <math.h>
#include <string.h>
#include <time.h>
#include "sim_engine.h"
#include "../common/ipc_common.h"

#define PI  3.14159265358979323846

/* =========================================================
 * Private helpers
 * ========================================================= */

/* Deterministic "noise": small sinusoid at 7.3 Hz, amplitude A */
static float det_noise(uint64_t tick, uint32_t period_ms, float amplitude)
{
    double t_s = (double)tick * (double)period_ms * 0.001;
    return (float)(amplitude * sin(2.0 * PI * 7.3 * t_s));
}

/*
 * flow_waveform()
 *   Models spirometric airflow using a half-sine approximation:
 *     Inspiration (+): peak_flow × sin(π × phase / T_insp)
 *     Expiration  (−): peak_flow × sin(π × phase / T_exp)  [negated]
 *   I:E ratio = 1:1.5  (typical ventilated patient)
 */
static float flow_waveform(uint32_t phase_tick,
                            uint32_t insp_ticks,
                            uint32_t exp_ticks,
                            float peak_flow_lpm)
{
    if (phase_tick < insp_ticks) {
        /* Inspiration */
        double theta = PI * (double)phase_tick / (double)insp_ticks;
        return (float)(peak_flow_lpm * sin(theta));
    } else {
        /* Expiration */
        uint32_t exp_phase = phase_tick - insp_ticks;
        double theta = PI * (double)exp_phase / (double)exp_ticks;
        return -(float)(peak_flow_lpm * sin(theta));
    }
}

/*
 * co2_waveform()
 *   Piecewise capnography model (4-phase):
 *     Phase I  (dead-space, first 15% of expiration): CO2 ≈ 0
 *     Phase II (rapid rise,  next 20% of expiration): linear 0 → EtCO2
 *     Phase III (alveolar plateau, remaining expiration): EtCO2 + upslope
 *     Phase 0  (inspiration): exponential washout back to FiCO2
 */
static float co2_waveform(uint32_t phase_tick,
                           uint32_t insp_ticks,
                           uint32_t exp_ticks,
                           float etco2,
                           float fico2)
{
    if (phase_tick < insp_ticks) {
        /* Inspiration — washout toward FiCO2 */
        double frac = (double)phase_tick / (double)insp_ticks;
        /* Exponential decay from etco2 (lingering) down to fico2 */
        return (float)(etco2 * exp(-5.0 * frac) + fico2 * (1.0 - exp(-5.0 * frac)));
    }

    uint32_t exp_phase = phase_tick - insp_ticks;
    uint32_t ph1_end = (uint32_t)(0.15 * exp_ticks);  /* dead-space   */
    uint32_t ph2_end = (uint32_t)(0.35 * exp_ticks);  /* rapid rise   */

    if (exp_phase < ph1_end) {
        /* Phase I: dead-space, CO2 near baseline */
        return fico2;
    } else if (exp_phase < ph2_end) {
        /* Phase II: linear rise */
        double frac = (double)(exp_phase - ph1_end) / (double)(ph2_end - ph1_end);
        return (float)(fico2 + (etco2 - fico2) * frac);
    } else {
        /* Phase III: alveolar plateau with slight upslope (0.5 mmHg rise) */
        double frac = (double)(exp_phase - ph2_end) / (double)(exp_ticks - ph2_end);
        return (float)(etco2 + 0.5 * frac);
    }
}

/*
 * pressure_waveform()
 *   Inspiration: PEEP → PIP via raised cosine (smooth ramp)
 *   Expiration:  exponential decay from PIP back to PEEP
 */
static float pressure_waveform(uint32_t phase_tick,
                                uint32_t insp_ticks,
                                uint32_t exp_ticks,
                                float pip,
                                float peep)
{
    if (phase_tick < insp_ticks) {
        double frac = (double)phase_tick / (double)insp_ticks;
        /* Raised cosine: smooth 0→1 */
        double shape = 0.5 * (1.0 - cos(PI * frac));
        return (float)(peep + (pip - peep) * shape);
    } else {
        uint32_t exp_phase = phase_tick - insp_ticks;
        double frac = (double)exp_phase / (double)exp_ticks;
        /* Exponential decay toward PEEP */
        return (float)(peep + (pip - peep) * exp(-4.0 * frac));
    }
}

/* =========================================================
 * Public API Implementation
 * ========================================================= */

void sim_init(sim_state_t *s, uint32_t tick_period_ms, float rr_bpm)
{
    memset(s, 0, sizeof(*s));

    /* Breath timing */
    float period_ms     = 60000.0f / rr_bpm;             /* ms per breath   */
    s->breath_period_ticks = (uint32_t)(period_ms / tick_period_ms);
    s->insp_ticks  = (uint32_t)(0.40f * s->breath_period_ticks); /* I:E 1:1.5 */
    s->exp_ticks   = s->breath_period_ticks - s->insp_ticks;

    /* Capnography baseline (normal adult under general anaesthesia) */
    s->etco2_baseline  = 38.0f;   /* mmHg — normal end-tidal CO2    */
    s->etco2_current   = 38.0f;
    s->fico2_baseline  =  1.5f;   /* mmHg — minimal rebreathing     */

    /* Oxygen */
    s->fio2_baseline   = 40.0f;   /* % — elevated FiO2 for GA       */

    /* N2O */
    s->fin2o_baseline  = 50.0f;   /* % — standard N2O/O2 mixture    */

    /* Anesthetic agent (e.g. Sevoflurane, MAC = 2.05%) */
    s->fiaa_baseline   =  2.0f;   /* % — ~1 × MAC                   */
    s->mac_baseline    =  2.05f;

    /* Pressure */
    s->pip_baseline    = 20.0f;   /* cmH2O                          */
    s->peep_baseline   =  5.0f;   /* cmH2O                          */

    /* Environment */
    s->gas_temp_c      = 37.0f;
    s->humidity_pct    = 100.0f;  /* fully humidified breathing gas  */
    s->atm_pressure_pa = 101325.0f;

    s->fault  = FAULT_NONE;
    s->seq_num = 0;
}

void sim_step(sim_state_t *s, sensor_data_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type = MSG_TYPE_SENSOR_DATA;

    /* ---- FAULT: sensor freeze ----------------------------------------- */
    if (s->fault == FAULT_SENSOR_FREEZE) {
        /* Return last good sample with same seq_num (stopped advancing)    */
        *out = s->last_good;
        out->sensor_frozen = 1;
        /* Advance tick so process timing isn't affected                    */
        s->tick++;
        return;
    }

    /* ---- Advance simulation clock -------------------------------------- */
    s->tick++;
    s->seq_num++;
    s->phase_tick = s->tick % s->breath_period_ticks;

    /* ---- FAULT: apnea — zero all respiratory signals ------------------- */
    if (s->fault == FAULT_APNEA) {
        out->timestamp_ns          = now_ns();
        out->seq_num               = s->seq_num;
        out->flow_lpm              = 0.0f;
        out->airway_pressure_cmH2O = s->peep_baseline;
        out->co2_waveform_mmhg     = s->fico2_baseline; /* CO2 flatlines    */
        out->fico2_mmhg            = s->fico2_baseline;
        out->fio2_pct              = s->fio2_baseline;
        out->fin2o_pct             = s->fin2o_baseline;
        out->fiaa_pct              = s->fiaa_baseline;
        out->gas_temp_c            = s->gas_temp_c;
        out->humidity_pct          = s->humidity_pct;
        out->atm_pressure_pa       = s->atm_pressure_pa;
        out->apnea_active          = 1;
        s->apnea_ticks++;
        return;
    }

    /* ---- Normal waveform generation ------------------------------------ */
    uint32_t ph  = (uint32_t)s->phase_tick;
    uint32_t iT  = s->insp_ticks;
    uint32_t eT  = s->exp_ticks;

    /* Flow — peak 30 L/min, physiologically reasonable for adult */
    float flow = flow_waveform(ph, iT, eT, 30.0f);
    flow += det_noise(s->tick, 20, 0.3f);  /* ±0.3 L/min noise */

    /* Pressure */
    float paw = pressure_waveform(ph, iT, eT,
                                   s->pip_baseline, s->peep_baseline);
    paw += det_noise(s->tick, 20, 0.2f);

    /* CO2 capnography */
    float co2 = co2_waveform(ph, iT, eT,
                               s->etco2_baseline, s->fico2_baseline);
    co2 += det_noise(s->tick, 20, 0.15f);
    if (co2 < 0.0f) co2 = 0.0f;

    /* FiO2 — slow sinusoidal drift (±2% over 60 s) to exercise alarm */
    float t_s = (float)s->tick * 0.020f;  /* time in seconds (20 ms ticks) */
    float fio2 = s->fio2_baseline + 2.0f * sinf(2.0f * (float)PI * t_s / 60.0f);

    /* N2O — stable with small noise */
    float fin2o = s->fin2o_baseline + det_noise(s->tick, 20, 0.2f);

    /* Anesthetic agent — very slow rise to test RAPID_AA_CHANGE alarm     */
    float fiaa = s->fiaa_baseline + 0.001f * (float)(s->tick % 500);
    if (fiaa > 4.0f) fiaa = s->fiaa_baseline;  /* reset to prevent runaway */

    /* Fill output structure */
    out->timestamp_ns          = now_ns();
    out->seq_num               = s->seq_num;
    out->flow_lpm              = flow;
    out->airway_pressure_cmH2O = paw;
    out->co2_waveform_mmhg     = co2;
    out->fico2_mmhg            = s->fico2_baseline + det_noise(s->tick, 20, 0.05f);
    out->fio2_pct              = fio2;
    out->fin2o_pct             = fin2o;
    out->fiaa_pct              = fiaa;
    out->gas_temp_c            = s->gas_temp_c   + det_noise(s->tick, 20, 0.05f);
    out->humidity_pct          = s->humidity_pct + det_noise(s->tick, 20, 0.1f);
    out->atm_pressure_pa       = s->atm_pressure_pa;
    out->sensor_frozen         = 0;
    out->apnea_active          = 0;

    /* Save as last good sample (used if SENSOR_FREEZE is injected later) */
    s->last_good = *out;
    s->last_good.sensor_frozen = 0;   /* mark saved copy as good */
}

void sim_inject_fault(sim_state_t *s, fault_scenario_t fault)
{
    s->fault       = fault;
    s->apnea_ticks = 0;
}
