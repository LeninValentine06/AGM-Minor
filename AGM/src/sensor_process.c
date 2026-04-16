/*
 * sensor_process.c  —  AGM Sensor Simulation Process
 *
 * ROLE:    Sensor Layer (Layer 1)
 * PERIOD:  20 ms (50 Hz) — deterministic periodic task
 * IPC OUT: MsgSend(sensor_data_t) → compute_process
 *          MsgSendPulse(WD_FEED)  → watchdog_process
 *
 * TIMING MODEL (drift prevention):
 *   Instead of sleeping for a fixed 20 ms after each iteration,
 *   we compute the ABSOLUTE next wake-up time and use TIMER_ABSTIME.
 *   Any processing time inside the loop is automatically absorbed.
 *   This guarantees a jitter-free 20 ms cadence over time.
 *
 * FAULT INJECTION SCHEDULE (demo):
 *   t =  0–30 s   : Normal operation
 *   t = 30–60 s   : APNEA injected  (flow = 0, CO2 flat)
 *   t = 60–90 s   : SENSOR FREEZE   (seq_num stops, safety→sensor_fail alarm)
 *   t = 90+   s   : Return to normal
 *
 * VIVA NOTE — Why processes, not threads?
 *   If sensor_process crashes, the QNX microkernel terminates only that
 *   process.  compute_process continues running and the watchdog detects
 *   the missing heartbeat within WD_TIMEOUT_NS.  With threads, one
 *   runaway thread can corrupt the entire address space.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>

#include "common/agm_types.h"
#include "common/ipc_common.h"
#include "sensor/sim_engine.h"

/* =========================================================
 * Fault injection schedule
 * ========================================================= */
typedef struct {
    float              t_start_s;
    float              t_end_s;
    fault_scenario_t   fault;
    const char        *label;
} fault_schedule_t;

static const fault_schedule_t FAULT_SCHEDULE[] = {
    {  0.0f,  30.0f, FAULT_NONE,          "NORMAL OPERATION"  },
    { 30.0f,  60.0f, FAULT_APNEA,         "APNEA INJECTED"    },
    { 60.0f,  90.0f, FAULT_SENSOR_FREEZE, "SENSOR FREEZE"     },
    { 90.0f, 999.0f, FAULT_NONE,          "RECOVERY / NORMAL" },
};
#define SCHEDULE_LEN  (sizeof(FAULT_SCHEDULE)/sizeof(FAULT_SCHEDULE[0]))

/* =========================================================
 * Main
 * ========================================================= */
int main(void)
{
    printf("[sensor_process] PID %d starting\n", getpid());

    /* ---- Connect to compute_process ----------------------------------- */
    printf("[sensor_process] Waiting for compute_process channel...\n");
    int compute_coid = name_open_retry(AGM_COMPUTE_CHANNEL, 50);
    if (compute_coid == -1) {
        fprintf(stderr, "[sensor_process] FATAL: cannot connect to compute: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    printf("[sensor_process] Connected to compute_process (coid=%d)\n",
           compute_coid);

    /* ---- Connect to watchdog_process ---------------------------------- */
    int wd_coid = name_open_retry(AGM_WATCHDOG_CHANNEL, 30);
    if (wd_coid == -1) {
        fprintf(stderr, "[sensor_process] WARNING: no watchdog found\n");
        /* Not fatal — watchdog is optional for minimal demo */
    }

    /* ---- Initialise simulation engine --------------------------------- */
    sim_state_t sim;
    sim_init(&sim, 20 /* ms */, 15.0f /* breaths/min */);
    printf("[sensor_process] Simulation engine initialised (15 BPM, 20 ms tick)\n");

    /* ---- Set up periodic timing --------------------------------------- */
    struct timespec next_wake;
    clock_gettime(CLOCK_MONOTONIC, &next_wake);

    msg_reply_t reply;
    sensor_data_t sample;
    uint32_t current_schedule = 0;
    uint64_t start_ns = now_ns();

    printf("[sensor_process] Entering main loop — scenario: %s\n",
           FAULT_SCHEDULE[0].label);

    /* ================================================================
     * MAIN SENSOR LOOP — 20 ms periodic
     * ================================================================ */
    for (;;) {
        /* ---- Advance absolute wake-up time ----------------------------- */
        timespec_add_ns(&next_wake, SENSOR_PERIOD_NS);

        /* ---- Update fault schedule ------------------------------------- */
        float elapsed_s = (float)(now_ns() - start_ns) * 1e-9f;
        if (current_schedule < SCHEDULE_LEN - 1 &&
            elapsed_s >= FAULT_SCHEDULE[current_schedule + 1].t_start_s)
        {
            current_schedule++;
            fault_scenario_t new_fault = FAULT_SCHEDULE[current_schedule].fault;
            sim_inject_fault(&sim, new_fault);
            printf("\n[sensor_process] *** SCENARIO CHANGE @ t=%.1fs: %s ***\n\n",
                   elapsed_s, FAULT_SCHEDULE[current_schedule].label);
        }

        /* ---- Simulate COMPUTE_DELAY fault ------------------------------ */
        if (sim.fault == FAULT_COMPUTE_DELAY) {
            struct timespec delay = { 0, 30000000L }; /* 30 ms extra lag */
            nanosleep(&delay, NULL);
        }

        /* ---- Generate sensor sample ------------------------------------ */
        sim_step(&sim, &sample);

        /* ---- Send to compute_process (blocking MsgSend) ---------------- */
        /*
         * VIVA NOTE — MsgSend semantics:
         *   This call blocks sensor_process until compute_process calls
         *   MsgReply().  This provides natural back-pressure: if compute
         *   falls behind, sensor will also slow down (flow control).
         *   Priority inheritance ensures compute inherits sensor's priority
         *   if compute is preempted while holding the reply.
         */
        int rc = MsgSend(compute_coid,
                         &sample,  sizeof(sample),
                         &reply,   sizeof(reply));
        if (rc == -1) {
            fprintf(stderr, "[sensor_process] MsgSend failed: %s\n",
                    strerror(errno));
            /* compute_process may have restarted — attempt reconnect */
            ConnectDetach(compute_coid);
            compute_coid = name_open_retry(AGM_COMPUTE_CHANNEL, 5);
            if (compute_coid == -1) {
                fprintf(stderr, "[sensor_process] Cannot reconnect. Exiting.\n");
                break;
            }
        }

        /* ---- Heartbeat to watchdog ------------------------------------- */
        if (wd_coid != -1) {
            MsgSendPulse(wd_coid, -1, PULSE_CODE_WD_FEED, WD_ID_SENSOR);
        }

        /* ---- Sleep until next absolute time (drift-free) -------------- */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wake, NULL);
    }

    ConnectDetach(compute_coid);
    if (wd_coid != -1) ConnectDetach(wd_coid);
    printf("[sensor_process] Exiting.\n");
    return EXIT_SUCCESS;
}
