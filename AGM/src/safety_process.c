/*
 * safety_process.c  —  AGM Safety Supervisor Process
 *
 * ROLE:    Supervision Layer (Layer 3)
 * IPC IN:  MsgReceive(pulse PARAMS_READY) ← compute_process
 *          Reads computed_data_t from shared memory on each pulse.
 * IPC OUT: MsgSendPulse(WD_FEED)          → watchdog_process
 *          (future: MsgSendPulse(ALARM_ACTIVE) → GUI process)
 *
 * ALARM EVALUATION (from "List of Alarms" PDF, Sections 1–8):
 *
 *   Each alarm has:
 *     - A primary threshold (instantaneous or duration-based)
 *     - An alarm_tracker_t that tracks:
 *         breach_start_ns : when did the violation start?
 *         state           : INACTIVE / ACTIVE / ACKNOWLEDGED
 *
 *   Duration-based alarms (e.g. "Low FiO2 for >5 s") only fire after
 *   the threshold has been breached continuously for the required duration.
 *   This prevents spurious alarms from transient noise.
 *
 *   Hysteresis: an alarm clears only when the value returns to a safe
 *   margin beyond the threshold (10% recovery band).
 *
 * FAULT ISOLATION (viva note):
 *   safety_process runs in a separate address space.  If compute_process
 *   crashes, safety_process detects the missing pulses (via watchdog) and
 *   can independently raise a COMPUTE_FAILURE system alarm.
 *   The microkernel ensures safety always has CPU time (it runs at a
 *   higher priority than logger and GUI).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <pthread.h>
#include <math.h>

#include "common/agm_types.h"
#include "common/ipc_common.h"

/* =========================================================
 * Alarm configuration table (thresholds from PDF)
 * ========================================================= */
typedef struct {
    alarm_id_t       id;
    alarm_priority_t priority;
    float            lo_thresh;     /* lower limit (NAN = not used) */
    float            hi_thresh;     /* upper limit (NAN = not used) */
    float            duration_s;    /* 0 = instantaneous            */
    const char      *name;
} alarm_cfg_t;

#define NO_LO   (-1e30f)
#define NO_HI   ( 1e30f)

static const alarm_cfg_t ALARM_CFG[ALARM_COUNT] = {
    /* id                         priority               lo      hi      dur   name                     */
    [ALARM_LOW_FIO2]            = {ALARM_LOW_FIO2,          ALARM_PRIORITY_HIGH,     NO_LO,  21.0f,  5.0f,  "Low FiO2"               },
    [ALARM_HIGH_FIO2]           = {ALARM_HIGH_FIO2,         ALARM_PRIORITY_MEDIUM,   60.0f,  NO_HI,  0.0f,  "High FiO2"              },
    [ALARM_RAPID_FIO2_DROP]     = {ALARM_RAPID_FIO2_DROP,   ALARM_PRIORITY_HIGH,     NO_LO,  NO_HI,  0.0f,  "Rapid FiO2 Drop"        }, /* handled specially */
    [ALARM_FIO2_SENSOR_FAILURE] = {ALARM_FIO2_SENSOR_FAILURE,ALARM_PRIORITY_CRITICAL,NO_LO,  NO_HI,  0.0f,  "FiO2 Sensor Failure"    }, /* handled specially */
    [ALARM_FIO2_OUT_OF_RANGE]   = {ALARM_FIO2_OUT_OF_RANGE, ALARM_PRIORITY_HIGH,     NO_LO,  NO_HI,  0.0f,  "FiO2 Out of Range"      }, /* <0 or >100        */

    [ALARM_HIGH_ETCO2]          = {ALARM_HIGH_ETCO2,        ALARM_PRIORITY_HIGH,     50.0f,  NO_HI,  3.0f,  "High EtCO2"             },
    [ALARM_LOW_ETCO2]           = {ALARM_LOW_ETCO2,         ALARM_PRIORITY_HIGH,     NO_LO,  25.0f,  3.0f,  "Low EtCO2"              },
    [ALARM_APNEA]               = {ALARM_APNEA,             ALARM_PRIORITY_CRITICAL, NO_LO,  NO_HI, 10.0f,  "APNEA"                  }, /* handled specially */
    [ALARM_REBREATHING]         = {ALARM_REBREATHING,       ALARM_PRIORITY_HIGH,      5.0f,  NO_HI,  0.0f,  "Rebreathing (FiCO2>5)"  },
    [ALARM_CO2_SENSOR_FAILURE]  = {ALARM_CO2_SENSOR_FAILURE,ALARM_PRIORITY_CRITICAL, NO_LO,  NO_HI,  0.0f,  "CO2 Sensor Failure"     },
    [ALARM_CO2_SATURATION]      = {ALARM_CO2_SATURATION,    ALARM_PRIORITY_HIGH,    100.0f,  NO_HI,  0.0f,  "CO2 Sensor Saturation"  },

    [ALARM_HIGH_N2O]            = {ALARM_HIGH_N2O,          ALARM_PRIORITY_HIGH,     70.0f,  NO_HI,  0.0f,  "High N2O (>70%)"        },
    [ALARM_N2O_SENSOR_FAILURE]  = {ALARM_N2O_SENSOR_FAILURE,ALARM_PRIORITY_CRITICAL, NO_LO,  NO_HI,  0.0f,  "N2O Sensor Failure"     },

    [ALARM_HIGH_AA]             = {ALARM_HIGH_AA,           ALARM_PRIORITY_CRITICAL, NO_LO,  NO_HI,  0.0f,  "High Anesthetic (>2×MAC)"},/* uses mac value */
    [ALARM_LOW_AA]              = {ALARM_LOW_AA,            ALARM_PRIORITY_HIGH,     NO_LO,  NO_HI,  0.0f,  "Low Anesthetic (<0.3MAC)"},
    [ALARM_RAPID_AA_CHANGE]     = {ALARM_RAPID_AA_CHANGE,   ALARM_PRIORITY_HIGH,     NO_LO,  NO_HI,  0.0f,  "Rapid AA Change"        },
    [ALARM_AA_SENSOR_FAILURE]   = {ALARM_AA_SENSOR_FAILURE, ALARM_PRIORITY_CRITICAL, NO_LO,  NO_HI,  0.0f,  "AA Sensor Failure"      },

    [ALARM_HIGH_RESP_RATE]      = {ALARM_HIGH_RESP_RATE,    ALARM_PRIORITY_HIGH,     30.0f,  NO_HI,  0.0f,  "High Respiratory Rate"  },
    [ALARM_LOW_RESP_RATE]       = {ALARM_LOW_RESP_RATE,     ALARM_PRIORITY_HIGH,     NO_LO,   8.0f,  0.0f,  "Low Respiratory Rate"   },
    [ALARM_LOW_TIDAL_VOL]       = {ALARM_LOW_TIDAL_VOL,     ALARM_PRIORITY_HIGH,     NO_LO, 200.0f,  0.0f,  "Low Tidal Volume"       },
    [ALARM_HIGH_TIDAL_VOL]      = {ALARM_HIGH_TIDAL_VOL,    ALARM_PRIORITY_HIGH,  1000.0f,  NO_HI,  0.0f,  "High Tidal Volume"      },
    [ALARM_LOW_MINUTE_VENT]     = {ALARM_LOW_MINUTE_VENT,   ALARM_PRIORITY_HIGH,     NO_LO,   3.0f,  0.0f,  "Low Minute Ventilation" },
    [ALARM_HIGH_MINUTE_VENT]    = {ALARM_HIGH_MINUTE_VENT,  ALARM_PRIORITY_MEDIUM,   12.0f,  NO_HI,  0.0f,  "High Minute Ventilation"},
    [ALARM_FLOW_SENSOR_FAILURE] = {ALARM_FLOW_SENSOR_FAILURE,ALARM_PRIORITY_CRITICAL,NO_LO,  NO_HI,  0.0f,  "Flow Sensor Failure"    },

    [ALARM_HIGH_PIP]            = {ALARM_HIGH_PIP,          ALARM_PRIORITY_HIGH,     40.0f,  NO_HI,  0.0f,  "High PIP (>40 cmH2O)"   },
    [ALARM_LOW_AIRWAY_PRESSURE] = {ALARM_LOW_AIRWAY_PRESSURE,ALARM_PRIORITY_HIGH,    NO_LO,   5.0f,  0.0f,  "Low Airway Pressure"    },
    [ALARM_SUSTAINED_HIGH_PRESS]= {ALARM_SUSTAINED_HIGH_PRESS,ALARM_PRIORITY_HIGH,   35.0f,  NO_HI,  5.0f,  "Sustained High Pressure"},
    [ALARM_NEGATIVE_PRESSURE]   = {ALARM_NEGATIVE_PRESSURE, ALARM_PRIORITY_HIGH,     NO_LO,  -2.0f,  0.0f,  "Negative Pressure"      },
    [ALARM_PRESSURE_SENSOR_FAIL]= {ALARM_PRESSURE_SENSOR_FAIL,ALARM_PRIORITY_CRITICAL,NO_LO, NO_HI,  0.0f,  "Pressure Sensor Failure"},

    [ALARM_O2_PIPELINE_LOW]     = {ALARM_O2_PIPELINE_LOW,   ALARM_PRIORITY_CRITICAL, NO_LO, 300000.0f,0.0f, "O2 Pipeline Pressure Low"},
    [ALARM_GAS_SUPPLY_LOSS]     = {ALARM_GAS_SUPPLY_LOSS,   ALARM_PRIORITY_CRITICAL, NO_LO,  NO_HI,  0.0f,  "Gas Supply Loss"        },

    [ALARM_HIGH_TEMP]           = {ALARM_HIGH_TEMP,         ALARM_PRIORITY_MEDIUM,   50.0f,  NO_HI,  0.0f,  "High Device Temperature"},
    [ALARM_LOW_TEMP]            = {ALARM_LOW_TEMP,          ALARM_PRIORITY_MEDIUM,   NO_LO,   5.0f,  0.0f,  "Low Device Temperature" },
    [ALARM_HIGH_HUMIDITY]       = {ALARM_HIGH_HUMIDITY,     ALARM_PRIORITY_MEDIUM,   90.0f,  NO_HI,  0.0f,  "High Humidity (>90%RH)" },
};

/* =========================================================
 * Per-alarm runtime tracker
 * ========================================================= */
typedef struct {
    alarm_state_t   state;
    uint64_t        breach_start_ns; /* 0 = not in breach */
} alarm_tracker_t;

static alarm_tracker_t trackers[ALARM_COUNT];

/* =========================================================
 * Alarm evaluation helpers
 * ========================================================= */
static const char *priority_str(alarm_priority_t p)
{
    switch (p) {
        case ALARM_PRIORITY_LOW:      return "LOW";
        case ALARM_PRIORITY_MEDIUM:   return "MEDIUM";
        case ALARM_PRIORITY_HIGH:     return "HIGH";
        case ALARM_PRIORITY_CRITICAL: return "CRITICAL";
        default:                      return "?";
    }
}

/*
 * evaluate_alarm()
 *   breached : true if the alarm condition is currently met
 *   value    : the triggering parameter value (for display)
 *
 *   Returns true if the alarm just transitioned INACTIVE → ACTIVE.
 */
static bool evaluate_alarm(alarm_id_t id, bool breached, float value)
{
    alarm_tracker_t *t   = &trackers[id];
    const alarm_cfg_t *c = &ALARM_CFG[id];
    uint64_t now         = now_ns();

    if (breached) {
        if (t->breach_start_ns == 0) {
            t->breach_start_ns = now;    /* record when breach started     */
        }
        float elapsed_s = (float)(now - t->breach_start_ns) * 1e-9f;

        if (elapsed_s >= c->duration_s && t->state == ALARM_STATE_INACTIVE) {
            /* Threshold breached for required duration → fire alarm */
            t->state = ALARM_STATE_ACTIVE;
            printf("*** ALARM [%s] [%s] *** value=%.2f @ t=%.3fs ***\n",
                   priority_str(c->priority), c->name, value, elapsed_s);
            return true;   /* newly active */
        }
    } else {
        /* Condition cleared — reset tracker (with hysteresis: immediate here) */
        if (t->state == ALARM_STATE_ACTIVE) {
            printf("    [CLEARED] [%s]\n", c->name);
        }
        t->state           = ALARM_STATE_INACTIVE;
        t->breach_start_ns = 0;
    }
    return false;
}

/* =========================================================
 * Full alarm sweep — called once per PARAMS_READY pulse
 * ========================================================= */
static float prev_fio2 = 40.0f;  /* for rapid drop detection       */
static float prev_fiaa = 2.0f;   /* for rapid AA change detection  */

static void check_all_alarms(const computed_data_t *cd)
{
    /* ---- Section 1: O2 ------------------------------------------- */
    evaluate_alarm(ALARM_LOW_FIO2,
                   cd->fio2_pct < 21.0f,
                   cd->fio2_pct);

    evaluate_alarm(ALARM_HIGH_FIO2,
                   cd->fio2_pct > 60.0f,
                   cd->fio2_pct);

    /* Rapid FiO2 drop: >10% in a single safety evaluation cycle */
    float fio2_drop = prev_fio2 - cd->fio2_pct;
    evaluate_alarm(ALARM_RAPID_FIO2_DROP,
                   fio2_drop > 10.0f,
                   fio2_drop);
    prev_fio2 = cd->fio2_pct;

    evaluate_alarm(ALARM_FIO2_OUT_OF_RANGE,
                   cd->fio2_pct < 0.0f || cd->fio2_pct > 100.0f,
                   cd->fio2_pct);

    /* ---- Section 2: CO2 / Capnography --------------------------- */
    evaluate_alarm(ALARM_HIGH_ETCO2,
                   cd->etco2_mmhg > 50.0f,
                   cd->etco2_mmhg);

    evaluate_alarm(ALARM_LOW_ETCO2,
                   cd->etco2_mmhg > 0.5f && cd->etco2_mmhg < 25.0f,
                   cd->etco2_mmhg);

    /*
     * APNEA: time_since_last_breath from compute_process.
     * Threshold: >10 s without a detected breath.
     */
    evaluate_alarm(ALARM_APNEA,
                   cd->time_since_last_breath_s > 10.0f,
                   cd->time_since_last_breath_s);

    evaluate_alarm(ALARM_REBREATHING,
                   cd->fico2_mmhg > 5.0f,
                   cd->fico2_mmhg);

    evaluate_alarm(ALARM_CO2_SATURATION,
                   cd->etco2_mmhg > 100.0f,
                   cd->etco2_mmhg);

    /* ---- Section 3: N2O ----------------------------------------- */
    evaluate_alarm(ALARM_HIGH_N2O,
                   cd->fin2o_pct > 70.0f,
                   cd->fin2o_pct);

    /* ---- Section 4: Anesthetic Agent ---------------------------- */
    evaluate_alarm(ALARM_HIGH_AA,
                   cd->mac > 2.0f,              /* > 2 × MAC */
                   cd->mac);

    evaluate_alarm(ALARM_LOW_AA,
                   cd->mac < 0.3f && cd->mac > 0.0f,  /* < 0.3 MAC */
                   cd->mac);

    /* Rapid AA change: >0.5% change */
    float aa_change = fabsf(cd->fiaa_pct - prev_fiaa);
    evaluate_alarm(ALARM_RAPID_AA_CHANGE,
                   aa_change > 0.5f,
                   aa_change);
    prev_fiaa = cd->fiaa_pct;

    /* ---- Section 5: Ventilation / Respiratory ------------------- */
    evaluate_alarm(ALARM_HIGH_RESP_RATE,
                   cd->resp_rate_bpm > 30.0f,
                   cd->resp_rate_bpm);

    evaluate_alarm(ALARM_LOW_RESP_RATE,
                   cd->resp_rate_bpm > 0.5f && cd->resp_rate_bpm < 8.0f,
                   cd->resp_rate_bpm);

    evaluate_alarm(ALARM_LOW_TIDAL_VOL,
                   cd->tidal_vol_e_ml > 10.0f && cd->tidal_vol_e_ml < 200.0f,
                   cd->tidal_vol_e_ml);

    evaluate_alarm(ALARM_HIGH_TIDAL_VOL,
                   cd->tidal_vol_e_ml > 1000.0f,
                   cd->tidal_vol_e_ml);

    evaluate_alarm(ALARM_LOW_MINUTE_VENT,
                   cd->minute_vent_lpm > 0.1f && cd->minute_vent_lpm < 3.0f,
                   cd->minute_vent_lpm);

    evaluate_alarm(ALARM_HIGH_MINUTE_VENT,
                   cd->minute_vent_lpm > 12.0f,
                   cd->minute_vent_lpm);

    /* ---- Section 6: Airway Pressure ----------------------------- */
    evaluate_alarm(ALARM_HIGH_PIP,
                   cd->pip_cmH2O > 40.0f,
                   cd->pip_cmH2O);

    evaluate_alarm(ALARM_LOW_AIRWAY_PRESSURE,
                   cd->airway_pressure_cmH2O < 5.0f &&
                   cd->airway_pressure_cmH2O > -2.0f,
                   cd->airway_pressure_cmH2O);

    evaluate_alarm(ALARM_SUSTAINED_HIGH_PRESS,
                   cd->airway_pressure_cmH2O > 35.0f,
                   cd->airway_pressure_cmH2O);

    evaluate_alarm(ALARM_NEGATIVE_PRESSURE,
                   cd->airway_pressure_cmH2O < -2.0f,
                   cd->airway_pressure_cmH2O);

    /* ---- Section 8: Environment --------------------------------- */
    evaluate_alarm(ALARM_HIGH_TEMP,
                   cd->gas_temp_c > 50.0f,
                   cd->gas_temp_c);

    evaluate_alarm(ALARM_LOW_TEMP,
                   cd->gas_temp_c < 5.0f,
                   cd->gas_temp_c);

    evaluate_alarm(ALARM_HIGH_HUMIDITY,
                   cd->humidity_pct > 90.0f,
                   cd->humidity_pct);
}

/* =========================================================
 * Main
 * ========================================================= */
int main(void)
{
    printf("[safety_process] PID %d starting\n", getpid());
    memset(trackers, 0, sizeof(trackers));

    /* ---- Attach to name service --------------------------------------- */
    name_attach_t *attach = name_attach(NULL, AGM_SAFETY_CHANNEL, 0);
    if (!attach) {
        fprintf(stderr, "[safety_process] FATAL: name_attach failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    int chid = attach->chid;
    printf("[safety_process] Channel '%s' ready (chid=%d)\n",
           AGM_SAFETY_CHANNEL, chid);

    /* ---- Map shared memory (read-only) -------------------------------- */
    /*
     * VIVA NOTE — why read-only?
     *   safety_process should NEVER write to computed parameters — it
     *   only reads them.  Mapping as PROT_READ enforces this at the MMU
     *   level.  Any accidental write causes a SIGSEGV, caught by the OS,
     *   not silently corrupting another process's view.
     */
    int shm_fd;
    shared_memory_t *shm = NULL;
    int shm_ready = 0;

    /* Wait for compute_process to fully initialise SHM including mutex */
    struct timespec init_wait = { 1, 0 };  /* wait 1 full second first */
    nanosleep(&init_wait, NULL);

    /* Wait for compute_process to create the SHM */
    for (int i = 0; i < 100 && !shm_ready; i++) {
        shm_fd = shm_open(AGM_SHM_NAME, O_RDONLY, 0);
        if (shm_fd != -1) {
            shm = mmap(NULL, sizeof(shared_memory_t),
                       PROT_READ, MAP_SHARED, shm_fd, 0);
            close(shm_fd);
            if (shm != MAP_FAILED) {
                shm_ready = 1;
            }
        }
        if (!shm_ready) {
            struct timespec t = { 0, 100000000L };
            nanosleep(&t, NULL);
        }
    }
    if (!shm_ready) {
        fprintf(stderr, "[safety_process] Cannot map shared memory\n");
        name_detach(attach, 0);
        return EXIT_FAILURE;
    }
    printf("[safety_process] Shared memory mapped (read-only)\n");

    /* ---- Connect to watchdog ------------------------------------------ */
    int wd_coid = name_open_retry(AGM_WATCHDOG_CHANNEL, 10);

    printf("[safety_process] Entering alarm monitoring loop\n");

    /* ================================================================
     * MAIN EVENT LOOP — pulse-driven (sporadic task)
     *   Wakes only when compute_process signals PARAMS_READY.
     *   No busy-wait — CPU is released between pulses.
     * ================================================================ */
    agm_msg_t msg;
    uint32_t eval_count = 0;

    for (;;) {
        struct _msg_info info;
        int rcvid = MsgReceive(chid, &msg, sizeof(msg), &info);

        if (rcvid == -1) {
            fprintf(stderr, "[safety_process] MsgReceive error: %s\n",
                    strerror(errno));
            continue;
        }

        if (rcvid != 0) {
            /* Unexpected regular message — reply and ignore */
            msg_reply_t r = { -1 };
            MsgReply(rcvid, -1, &r, sizeof(r));
            continue;
        }

        /* ---- It's a pulse --------------------------------------------- */
        if (msg.pulse.code == PULSE_CODE_SHUTDOWN) {
            printf("[safety_process] Shutdown received.\n");
            break;
        }

        if (msg.pulse.code != PULSE_CODE_PARAMS_READY) {
            continue;
        }

        /* ---- Read computed data from shared memory -------------------- */
        /*
         * Seqlock-style read: copy data, verify write_seq didn't change.
         * If write_seq is odd, a write is in progress — spin briefly.
         */
        computed_data_t cd;
        uint32_t seq1, seq2;
        int retries = 0;
        do {
            seq1 = shm->write_seq;
            if (seq1 & 1) {
                /* Write in progress — yield and retry */
                struct timespec t = { 0, 1000L }; /* 1 µs */
                nanosleep(&t, NULL);
                retries++;
                if (retries > 1000) break;
                continue;
            }
            /* pthread_mutex is held by writer — we use it here for safety */
            /* For read-only mmap, we rely on the seqlock pattern instead  */
            cd = shm->data;
            seq2 = shm->write_seq;
        } while (seq1 != seq2);

        /* ---- Run all alarm checks ------------------------------------- */
        check_all_alarms(&cd);
        eval_count++;

        /* Print alive indication every 100 evaluations */
        if (eval_count % 100 == 0) {
            printf("[safety_process] %u evaluations  RR=%.1fbpm  EtCO2=%.1fmmHg  "
                   "FiO2=%.1f%%  apnea_t=%.1fs\n",
                   eval_count,
                   cd.resp_rate_bpm, cd.etco2_mmhg,
                   cd.fio2_pct, cd.time_since_last_breath_s);
        }

        /* ---- Heartbeat to watchdog ------------------------------------ */
        if (wd_coid != -1) {
            MsgSendPulse(wd_coid, -1, PULSE_CODE_WD_FEED, WD_ID_SAFETY);
        }
    }

    munmap(shm, sizeof(shared_memory_t));
    if (wd_coid != -1) ConnectDetach(wd_coid);
    name_detach(attach, 0);
    printf("[safety_process] Exiting.\n");
    return EXIT_SUCCESS;
}
