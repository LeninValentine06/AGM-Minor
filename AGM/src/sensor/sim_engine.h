/*
 * sim_engine.h  —  Deterministic waveform simulation interface
 *
 * The simulation engine replaces physical sensors.
 * All waveforms are mathematically defined so the same seed
 * always produces the same sequence — this is the "deterministic"
 * guarantee required for a real-time simulation framework.
 *
 * Waveforms modelled:
 *   - Respiratory flow (sinusoidal, I:E = 1:1.5)
 *   - Capnography CO2 (piecewise: dead-space / rise / plateau / wash-out)
 *   - FiO2  (slow drift ± 2%)
 *   - Airway pressure (inspiration ramp + PEEP baseline)
 *   - N2O, Anesthetic Agent (constants with small noise)
 *   - Environment (temperature, humidity, pressure — slow drift)
 *
 * Fault scenarios (injected by sensor_process on a timed schedule):
 *   FAULT_NONE           — normal operation
 *   FAULT_APNEA          — flow → 0, CO2 flatlines, no breath detected
 *   FAULT_SENSOR_FREEZE  — last good sample repeated, seq_num stops
 *   FAULT_COMPUTE_DELAY  — artificial sleep in sensor loop (shows drift)
 */

#ifndef SIM_ENGINE_H
#define SIM_ENGINE_H

#include <stdint.h>
#include "../common/agm_types.h"

/* =========================================================
 * Fault scenario selector
 * ========================================================= */
typedef enum {
    FAULT_NONE          = 0,
    FAULT_APNEA         = 1,
    FAULT_SENSOR_FREEZE = 2,
    FAULT_COMPUTE_DELAY = 3
} fault_scenario_t;

/* =========================================================
 * Simulation engine state  (opaque to callers)
 * ========================================================= */
typedef struct {
    /* Timing */
    uint64_t        tick;               /* total ticks since sim_init()      */
    uint64_t        phase_tick;         /* tick within current breath cycle  */

    /* Breath cycle parameters */
    uint32_t        breath_period_ticks; /* total ticks per breath cycle     */
    uint32_t        insp_ticks;          /* ticks in inspiration phase       */
    uint32_t        exp_ticks;           /* ticks in expiration phase        */

    /* Capnography state */
    float           etco2_baseline;     /* normal EtCO2 [mmHg]              */
    float           etco2_current;      /* value at last breath end         */
    float           fico2_baseline;     /* normal FiCO2 [mmHg]              */

    /* O2 / N2O / Agent */
    float           fio2_baseline;
    float           fin2o_baseline;
    float           fiaa_baseline;      /* as percent                       */
    float           mac_baseline;       /* MAC for current agent            */

    /* Pressure */
    float           pip_baseline;       /* [cmH2O]                          */
    float           peep_baseline;      /* [cmH2O]                          */

    /* Environment */
    float           gas_temp_c;
    float           humidity_pct;
    float           atm_pressure_pa;

    /* Last good sample (for SENSOR_FREEZE fault) */
    sensor_data_t   last_good;

    /* Fault state */
    fault_scenario_t fault;
    uint32_t        apnea_ticks;        /* ticks spent in apnea             */

    /* Sequence counter */
    uint32_t        seq_num;
} sim_state_t;

/* =========================================================
 * Public API
 * ========================================================= */

/*
 * sim_init() — initialise engine with physiological defaults.
 *   tick_period_ms : the period at which sim_step() will be called (e.g. 20)
 *   rr_bpm         : simulated respiratory rate (e.g. 15)
 */
void sim_init(sim_state_t *s, uint32_t tick_period_ms, float rr_bpm);

/*
 * sim_step() — advance simulation by one tick and fill *out.
 *   Call once per sensor period (every 20 ms).
 */
void sim_step(sim_state_t *s, sensor_data_t *out);

/*
 * sim_inject_fault() — switch to a fault scenario immediately.
 *   Pass FAULT_NONE to return to normal.
 */
void sim_inject_fault(sim_state_t *s, fault_scenario_t fault);

#endif /* SIM_ENGINE_H */
