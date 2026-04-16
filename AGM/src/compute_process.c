/*
 * compute_process.c  —  AGM Parameter Computation Process
 *
 * ROLE:    Processing Layer (Layer 2)
 * IPC IN:  MsgReceive(sensor_data_t)   ← sensor_process      [synchronous]
 * IPC OUT: write → shared_memory_t     → safety + GUI future  [shared mem]
 *          MsgSendPulse(PARAMS_READY)  → safety_process       [async pulse]
 *          MsgSend(computed_data_t)    → logger_process        [1 Hz rate]
 *          MsgSendPulse(WD_FEED)       → watchdog_process
 *
 * PARAMETER COMPUTATION (25 total from Parameters Computed PDF):
 *
 *   Gas (10):    FiO2, EtO2, FiCO2, EtCO2, FiN2O, EtN2O, FiAA, EtAA, MAC, MAC-Age
 *   Respiratory: RR (zero-crossing), VTi/VTe (flow integration), MV, Flow
 *   Pressure:    Paw, PIP, PEEP, Pmean
 *   Derived:     Cdyn = VTi / (PIP - PEEP),  Cstat (simplified)
 *   Environment: T_gas, Humidity, P_atm
 *
 * BREATH DETECTION ALGORITHM:
 *   A new inspiration is detected when flow crosses zero from negative to
 *   positive (end of expiration → start of inspiration).
 *   Tidal volumes are computed by numerically integrating flow over each
 *   inspiration and expiration half-cycle.
 *   RR is derived from the period between successive zero-crossings.
 *
 * SHARED MEMORY SYNCHRONISATION:
 *   A PTHREAD_PROCESS_SHARED mutex with priority inheritance prevents
 *   partial reads by safety_process.  The write sequence counter (write_seq)
 *   is incremented before and after the write so readers can detect races.
 *
 * VIVA NOTE — Why send a pulse to safety, not a message?
 *   A pulse is a 8-byte kernel-level notification with no data payload.
 *   It does not block compute_process — compute continues immediately after
 *   MsgSendPulse().  safety_process then reads from shared memory at its own
 *   pace.  This decouples the two processes and keeps compute's 20 ms loop
 *   deterministic regardless of how long safety takes to evaluate alarms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <pthread.h>

#include "common/agm_types.h"
#include "common/ipc_common.h"

/* =========================================================
 * Constants
 * ========================================================= */
#define SENSOR_DT_S         0.020f      /* 20 ms sensor period in seconds   */
#define LOG_EVERY_N_TICKS   50          /* log at 1 Hz (50 × 20 ms = 1 s)   */
#define PATIENT_AGE         40          /* assumed patient age (years)       */
#define MAC_SEVOFLURANE     2.05f       /* MAC for sevoflurane [%]           */
#define ETO2_OFFSET         5.0f        /* typical O2 extraction [%]         */

/* =========================================================
 * Breath tracker — per-breath accumulator
 * ========================================================= */
typedef struct {
    bool        in_inspiration;         /* current phase flag               */
    float       prev_flow;              /* previous tick's flow (for crossing detection) */
    float       vti_accum_L;            /* accumulate VTi in litres         */
    float       vte_accum_L;            /* accumulate VTe in litres         */
    float       pip_current;            /* max Paw this breath              */
    float       peep_last;              /* PEEP = Paw just before insp start*/
    float       pmean_accum;            /* sum of Paw samples this breath   */
    uint32_t    pmean_count;
    uint64_t    breath_start_ns;        /* timestamp of last breath start   */
    uint64_t    last_complete_breath_ns;/* timestamp of last complete breath */
    float       breath_period_s;        /* smoothed breath period           */
    float       resp_rate_bpm;          /* derived RR                       */
    float       vti_ml;                 /* finalised VTi [mL]               */
    float       vte_ml;                 /* finalised VTe [mL]               */
    float       pmean_cmH2O;            /* mean airway pressure             */
    float       pip_cmH2O;
    float       peep_cmH2O;
    float       etco2_peak;             /* peak CO2 during expiration        */
    bool        etco2_tracking;         /* are we in expiration phase?       */
    uint32_t    breath_count;
} breath_tracker_t;

/* =========================================================
 * Breath detection and parameter update
 * ========================================================= */
static void breath_tracker_init(breath_tracker_t *bt)
{
    memset(bt, 0, sizeof(*bt));
    bt->resp_rate_bpm   = 15.0f;   /* initial estimate */
    bt->breath_period_s =  4.0f;
    bt->vti_ml          = 500.0f;
    bt->vte_ml          = 500.0f;
    bt->pip_cmH2O       = 20.0f;
    bt->peep_cmH2O      =  5.0f;
    bt->pmean_cmH2O     = 10.0f;
    bt->etco2_peak      = 38.0f;
    bt->last_complete_breath_ns = now_ns();
}

static void breath_tracker_update(breath_tracker_t *bt,
                                   const sensor_data_t *s)
{
    float flow = s->flow_lpm;
    float paw  = s->airway_pressure_cmH2O;
    float co2  = s->co2_waveform_mmhg;

    /* ---- Detect zero crossing: neg→pos = new inspiration begins ------- */
    bool new_breath = (bt->prev_flow <= 0.0f && flow > 0.5f);

    if (new_breath && bt->in_inspiration == false) {
        /* Finalise previous expiration */
        bt->vte_ml     = bt->vte_accum_L * 1000.0f;
        bt->pip_cmH2O  = bt->pip_current;
        bt->peep_cmH2O = bt->peep_last;
        if (bt->pmean_count > 0) {
            bt->pmean_cmH2O = bt->pmean_accum / (float)bt->pmean_count;
        }
        /* EtCO2 is the peak CO2 seen during expiration */
        /* (already tracked in bt->etco2_peak) */

        /* Compute breath period and RR */
        if (bt->breath_start_ns != 0) {
            uint64_t now = now_ns();
            float period_s = (float)(now - bt->breath_start_ns) * 1e-9f;
            /* Exponential moving average to smooth RR */
            bt->breath_period_s = 0.8f * bt->breath_period_s + 0.2f * period_s;
            if (bt->breath_period_s > 0.0f) {
                bt->resp_rate_bpm = 60.0f / bt->breath_period_s;
            }
            bt->last_complete_breath_ns = now;
        }
        bt->breath_start_ns   = now_ns();
        bt->breath_count++;

        /* Reset accumulators */
        bt->in_inspiration  = true;
        bt->vti_accum_L     = 0.0f;
        bt->vte_accum_L     = 0.0f;
        bt->pip_current     = paw;
        bt->peep_last       = bt->pmean_accum > 0
                              ? bt->peep_cmH2O : 5.0f; /* carry forward   */
        bt->pmean_accum     = 0.0f;
        bt->pmean_count     = 0;
        bt->etco2_tracking  = false;
        bt->etco2_peak      = 0.0f;
    }

    /* ---- Detect pos→neg crossing: start of expiration ----------------- */
    if (bt->prev_flow >= 0.0f && flow < -0.5f) {
        bt->in_inspiration = false;
        bt->vti_ml = bt->vti_accum_L * 1000.0f;
        bt->etco2_tracking = true;
    }

    /* ---- Integrate flow for tidal volumes ----------------------------- */
    /* flow [L/min] × dt [s] = dV [L] */
    if (flow > 0.0f) {
        bt->vti_accum_L += flow * SENSOR_DT_S / 60.0f;
    } else {
        bt->vte_accum_L += (-flow) * SENSOR_DT_S / 60.0f;
    }

    /* ---- Track peak pressure (PIP) ------------------------------------ */
    if (paw > bt->pip_current) bt->pip_current = paw;

    /* ---- Track PEEP (pressure at end of expiration) ------------------- */
    if (!bt->in_inspiration && flow > -0.5f && flow < 0.5f) {
        bt->peep_last = paw;
    }

    /* ---- Mean airway pressure ----------------------------------------- */
    bt->pmean_accum += paw;
    bt->pmean_count++;

    /* ---- EtCO2: track peak during expiration -------------------------- */
    if (bt->etco2_tracking && co2 > bt->etco2_peak) {
        bt->etco2_peak = co2;
    }

    bt->prev_flow = flow;
}

/* =========================================================
 * Main
 * ========================================================= */
int main(void)
{
    printf("[compute_process] PID %d starting\n", getpid());

    /* ---- Attach to name service --------------------------------------- */
    name_attach_t *attach = name_attach(NULL, AGM_COMPUTE_CHANNEL, 0);
    if (!attach) {
        fprintf(stderr, "[compute_process] FATAL: name_attach failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    int chid = attach->chid;
    printf("[compute_process] Channel '%s' ready (chid=%d)\n",
           AGM_COMPUTE_CHANNEL, chid);

    /* ---- Create / map shared memory ----------------------------------- */
    int shm_fd = shm_open(AGM_SHM_NAME, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (shm_fd == -1) {
        fprintf(stderr, "[compute_process] shm_open failed: %s\n",
                strerror(errno));
        name_detach(attach, 0);
        return EXIT_FAILURE;
    }
    if (ftruncate(shm_fd, sizeof(shared_memory_t)) == -1) {
        fprintf(stderr, "[compute_process] ftruncate failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    shared_memory_t *shm = mmap(NULL, sizeof(shared_memory_t),
                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                 shm_fd, 0);
    if (shm == MAP_FAILED) {
        fprintf(stderr, "[compute_process] mmap failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    close(shm_fd);

    /* ---- Initialise shared memory mutex (PROCESS_SHARED + prio-inherit) */
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&shm->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        shm->write_seq = 0;
    }
    printf("[compute_process] Shared memory '%s' initialised\n", AGM_SHM_NAME);

    /* ---- Connect to safety_process ------------------------------------ */
    printf("[compute_process] Waiting for safety_process...\n");
    int safety_coid = name_open_retry(AGM_SAFETY_CHANNEL, 50);
    if (safety_coid == -1) {
        fprintf(stderr, "[compute_process] WARNING: no safety process found\n");
    } else {
        printf("[compute_process] Connected to safety_process\n");
    }

    /* ---- Connect to logger_process ------------------------------------ */
    printf("[compute_process] Waiting for logger_process...\n");
    int logger_coid = name_open_retry(AGM_LOGGER_CHANNEL, 100);
    if (logger_coid == -1) {
        fprintf(stderr, "[compute_process] WARNING: no logger process found\n");
    } else {
        printf("[compute_process] Connected to logger_process\n");
    }

    /* ---- Connect to watchdog ------------------------------------------ */
    int wd_coid = name_open_retry(AGM_WATCHDOG_CHANNEL, 10);

    /* ---- Initialise breath tracker ------------------------------------ */
    breath_tracker_t bt;
    breath_tracker_init(&bt);

    agm_msg_t  msg;
    msg_reply_t reply = { 0 };
    uint32_t tick_count = 0;

    printf("[compute_process] Entering receive loop\n");

    /* ================================================================
     * MAIN RECEIVE LOOP
     * ================================================================ */
    for (;;) {
        struct _msg_info info;
        int rcvid = MsgReceive(chid, &msg, sizeof(msg), &info);

        if (rcvid == -1) {
            fprintf(stderr, "[compute_process] MsgReceive error: %s\n",
                    strerror(errno));
            continue;
        }

        if (rcvid == 0) {
            /* Pulse received (e.g. shutdown) */
            if (msg.pulse.code == PULSE_CODE_SHUTDOWN) {
                printf("[compute_process] Shutdown pulse received.\n");
                break;
            }
            continue;
        }

        /* ---- Process incoming sensor message -------------------------- */
        if (msg.hdr.type != MSG_TYPE_SENSOR_DATA) {
            MsgReply(rcvid, -1, &reply, sizeof(reply));
            continue;
        }

        /*
         * CRITICAL: reply to sensor_process FIRST so it can continue
         * its 20 ms loop.  All computation happens AFTER the reply.
         * This keeps the sensor's timing deterministic.
         */
        MsgReply(rcvid, 0, &reply, sizeof(reply));

        sensor_data_t *s = &msg.sensor;
        tick_count++;

        /* ---- Update breath tracking ----------------------------------- */
        breath_tracker_update(&bt, s);

        /* ---- Build computed_data_t ------------------------------------- */
        computed_data_t cd;
        memset(&cd, 0, sizeof(cd));
        cd.type = MSG_TYPE_COMPUTED_DATA;
        cd.timestamp_ns = s->timestamp_ns;
        cd.breath_count = bt.breath_count;

        /* Gas Monitoring (10 parameters) */
        cd.fio2_pct     = s->fio2_pct;
        cd.eto2_pct     = s->fio2_pct - ETO2_OFFSET;          /* approximation */
        cd.fico2_mmhg   = s->fico2_mmhg;
        cd.etco2_mmhg   = bt.etco2_peak;
        cd.fin2o_pct    = s->fin2o_pct;
        cd.etn2o_pct    = s->fin2o_pct * 0.95f;               /* ~5% extracted */
        cd.fiaa_pct     = s->fiaa_pct;
        cd.etaa_pct     = s->fiaa_pct * 0.90f;                /* ~10% extracted */
        cd.mac          = s->fiaa_pct / MAC_SEVOFLURANE;
        /* MAC-Age correction: MAC × (1 - 0.006 × (age - 40)) — ISO 8835 */
        cd.mac_age      = cd.mac * (1.0f - 0.006f * (float)(PATIENT_AGE - 40));

        /* Respiratory (5 parameters) */
        cd.resp_rate_bpm  = bt.resp_rate_bpm;
        cd.tidal_vol_i_ml = bt.vti_ml;
        cd.tidal_vol_e_ml = bt.vte_ml;
        cd.minute_vent_lpm = bt.resp_rate_bpm * bt.vte_ml / 1000.0f; /* L/min */
        cd.flow_rate_lpm  = s->flow_lpm;

        /* Pressure (4 parameters) */
        cd.airway_pressure_cmH2O = s->airway_pressure_cmH2O;
        cd.pip_cmH2O    = bt.pip_cmH2O;
        cd.peep_cmH2O   = bt.peep_cmH2O;
        cd.pmean_cmH2O  = bt.pmean_cmH2O;

        /* Derived (2 parameters) */
        float delta_p = bt.pip_cmH2O - bt.peep_cmH2O;
        if (delta_p > 0.1f) {
            cd.dynamic_compliance = bt.vti_ml / delta_p;
        } else {
            cd.dynamic_compliance = 0.0f; /* avoid divide by zero */
        }
        /* Static compliance: use VTe / (PIP - PEEP) as simplified estimate */
        cd.static_compliance = cd.dynamic_compliance * 0.85f;

        /* Environment (3 parameters) */
        cd.gas_temp_c      = s->gas_temp_c;
        cd.humidity_pct    = s->humidity_pct;
        cd.atm_pressure_pa = s->atm_pressure_pa;

        /* Apnea tracking */
        cd.time_since_last_breath_s =
            (float)(now_ns() - bt.last_complete_breath_ns) * 1e-9f;

        /* ---- Write to shared memory ----------------------------------- */
        pthread_mutex_lock(&shm->mutex);
        shm->write_seq++;              /* odd = write in progress           */
        shm->data = cd;
        shm->write_seq++;              /* even = write complete             */
        pthread_mutex_unlock(&shm->mutex);

        /* ---- Notify safety_process via pulse (non-blocking) ----------- */
        /*
         * VIVA NOTE — Pulse vs Message to safety:
         *   Using MsgSendPulse keeps compute's loop timing intact.
         *   safety_process picks up the pulse and reads from shared memory.
         *   If safety is slow for one cycle, the next pulse will "overwrite"
         *   the previous one in the kernel queue — safety always sees the
         *   most recent data, never stale data.
         */
        if (safety_coid != -1) {
            MsgSendPulse(safety_coid, -1, PULSE_CODE_PARAMS_READY, 0);
        }

        /* ---- Send to logger at 1 Hz (every LOG_EVERY_N_TICKS ticks) --- */
        if (logger_coid != -1 && (tick_count % LOG_EVERY_N_TICKS) == 0) {
            msg_reply_t log_reply;
            MsgSend(logger_coid,
                    &cd,        sizeof(cd),
                    &log_reply, sizeof(log_reply));
        }

        /* ---- Heartbeat to watchdog ------------------------------------ */
        if (wd_coid != -1) {
            MsgSendPulse(wd_coid, -1, PULSE_CODE_WD_FEED, WD_ID_COMPUTE);
        }
    }

    /* ---- Cleanup ------------------------------------------------------ */
    munmap(shm, sizeof(shared_memory_t));
    shm_unlink(AGM_SHM_NAME);
    if (safety_coid != -1) ConnectDetach(safety_coid);
    if (logger_coid  != -1) ConnectDetach(logger_coid);
    if (wd_coid      != -1) ConnectDetach(wd_coid);
    name_detach(attach, 0);
    printf("[compute_process] Exiting.\n");
    return EXIT_SUCCESS;
}
