/*
 * watchdog_process.c  —  AGM Hardware Watchdog Emulation
 *
 * ROLE:    Cross-cutting — monitors all other AGM processes
 * IPC IN:  MsgReceive(pulse WD_FEED, value = wd_process_id_t)
 *          Each process sends WD_FEED pulse every period.
 * OUTPUT:  Fault announcements on stdout.
 *          (future: trigger hardware watchdog reset register)
 *
 * MONITORING TABLE:
 *   Process         Expected feed interval    Timeout
 *   sensor_process      20 ms                 200 ms (10 × period)
 *   compute_process     20 ms                 200 ms
 *   safety_process     ~20 ms (pulse-driven)  200 ms
 *   logger_process    1000 ms                2000 ms
 *
 * ALGORITHM:
 *   1. Receive WD_FEED pulses — update last_feed_ns[process_id]
 *   2. On a 100 ms timer pulse — sweep all entries:
 *        if (now - last_feed > timeout) → FAULT
 *   3. If FAULT persists for WD_MAX_MISSED_FEEDS intervals:
 *        declare process DEAD → print fault
 *
 * VIVA NOTE — Why a dedicated watchdog process?
 *   In QNX, if a process hangs (blocked in a tight loop or deadlocked),
 *   it stops sending pulses.  The watchdog — running in a separate address
 *   space at HIGH priority — detects this and can initiate recovery actions.
 *   On real hardware this would kick a timer register that resets the MCU
 *   if not "fed" within the timeout window.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/siginfo.h>
#include <time.h>
#include <signal.h>

#include "common/agm_types.h"
#include "common/ipc_common.h"

/* =========================================================
 * Per-process watchdog entry
 * ========================================================= */
typedef struct {
    wd_process_id_t id;
    const char     *name;
    uint64_t        last_feed_ns;       /* timestamp of last WD_FEED pulse */
    uint64_t        timeout_ns;         /* per-process timeout             */
    uint32_t        missed_count;       /* consecutive missed feeds        */
    bool            registered;         /* has this process fed yet?       */
    bool            fault_declared;     /* have we announced this fault?   */
} wd_entry_t;

static wd_entry_t wd_table[WD_ID_COUNT] = {
    [WD_ID_SENSOR]  = { WD_ID_SENSOR,  "sensor_process",  0, 200000000ULL, 0, false, false },
    [WD_ID_COMPUTE] = { WD_ID_COMPUTE, "compute_process", 0, 200000000ULL, 0, false, false },
    [WD_ID_SAFETY]  = { WD_ID_SAFETY,  "safety_process",  0, 200000000ULL, 0, false, false },
    [WD_ID_LOGGER]  = { WD_ID_LOGGER,  "logger_process",  0, 2000000000ULL,0, false, false },
};

/* =========================================================
 * Timer pulse code for periodic sweep
 * ========================================================= */
#define PULSE_CODE_WD_TIMER  (_PULSE_CODE_MINAVAIL + 10)

/* =========================================================
 * Main
 * ========================================================= */
int main(void)
{
    printf("[watchdog_process] PID %d starting\n", getpid());

    /* ---- Attach to name service --------------------------------------- */
    name_attach_t *attach = name_attach(NULL, AGM_WATCHDOG_CHANNEL, 0);
    if (!attach) {
        fprintf(stderr, "[watchdog_process] FATAL: name_attach failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    int chid = attach->chid;
    printf("[watchdog_process] Channel '%s' ready (chid=%d)\n",
           AGM_WATCHDOG_CHANNEL, chid);

    /* ---- Set up periodic timer (100 ms) for sweep --------------------- */
    /*
     * QNX timer delivers a pulse to our channel — no signal, no thread.
     * This is the preferred real-time pattern: timers as pulses, not signals.
     */
    struct sigevent timer_event;
    int self_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (self_coid == -1) {
        fprintf(stderr, "[watchdog_process] ConnectAttach failed: %s\n",
                strerror(errno));
        name_detach(attach, 0);
        return EXIT_FAILURE;
    }

    SIGEV_PULSE_INIT(&timer_event, self_coid,
                     SIGEV_PULSE_PRIO_INHERIT,
                     PULSE_CODE_WD_TIMER, 0);

    timer_t tid;
    if (timer_create(CLOCK_MONOTONIC, &timer_event, &tid) == -1) {
        fprintf(stderr, "[watchdog_process] timer_create failed: %s\n",
                strerror(errno));
        name_detach(attach, 0);
        return EXIT_FAILURE;
    }

    struct itimerspec its;
    its.it_value.tv_sec  = 0;
    its.it_value.tv_nsec = (long)WD_CHECK_PERIOD_NS;
    its.it_interval      = its.it_value;
    timer_settime(tid, 0, &its, NULL);

    printf("[watchdog_process] Watchdog timer started (100 ms sweep)\n");

    /* Initialise last_feed timestamps so processes have time to start */
    uint64_t startup_ns = now_ns();
    for (int i = 0; i < WD_ID_COUNT; i++) {
        wd_table[i].last_feed_ns = startup_ns;
    }

    agm_msg_t msg;
    uint32_t sweep_count = 0;

    /* ================================================================
     * MAIN RECEIVE LOOP
     * ================================================================ */
    for (;;) {
        struct _msg_info info;
        int rcvid = MsgReceive(chid, &msg, sizeof(msg), &info);

        if (rcvid == -1) {
            fprintf(stderr, "[watchdog_process] MsgReceive error: %s\n",
                    strerror(errno));
            continue;
        }

        if (rcvid != 0) {
            /* Unexpected regular message */
            msg_reply_t r = { -1 };
            MsgReply(rcvid, -1, &r, sizeof(r));
            continue;
        }

        /* ---- Pulse received ------------------------------------------ */
        int8_t code  = msg.pulse.code;
        int    value = msg.pulse.value.sival_int;

        /* WD_FEED from a monitored process */
        if (code == PULSE_CODE_WD_FEED) {
            if (value >= 0 && value < WD_ID_COUNT) {
                wd_entry_t *e = &wd_table[value];
                e->last_feed_ns   = now_ns();
                e->missed_count   = 0;
                e->fault_declared = false;
                if (!e->registered) {
                    e->registered = true;
                    printf("[watchdog_process] Process registered: %s\n",
                           e->name);
                }
            }
            continue;
        }

        /* Timer sweep pulse */
        if (code == PULSE_CODE_WD_TIMER) {
            sweep_count++;
            uint64_t now = now_ns();
            bool any_fault = false;

            for (int i = 0; i < WD_ID_COUNT; i++) {
                wd_entry_t *e = &wd_table[i];

                /* Skip processes that haven't registered yet */
                if (!e->registered) continue;

                uint64_t elapsed = now - e->last_feed_ns;

                if (elapsed > e->timeout_ns) {
                    e->missed_count++;
                    any_fault = true;

                    if (!e->fault_declared) {
                        e->fault_declared = true;
                        printf("!!! WATCHDOG FAULT: %s missed feed "
                               "(last feed %.0f ms ago, timeout=%llu ms) !!!\n",
                               e->name,
                               (double)elapsed * 1e-6,
                               (unsigned long long)e->timeout_ns / 1000000ULL);
                    }

                    if (e->missed_count >= WD_MAX_MISSED_FEEDS) {
                        printf("!!! WATCHDOG CRITICAL: %s declared DEAD "
                               "(%u missed feeds) !!!\n",
                               e->name, e->missed_count);
                        /* In real system: trigger hardware reset or
                         * spawn replacement process here. */
                    }
                } else {
                    if (e->fault_declared) {
                        printf("[watchdog_process] %s RECOVERED\n", e->name);
                    }
                    e->fault_declared = false;
                    e->missed_count   = 0;
                }
            }

            /* Print alive summary every 10 sweeps (1 s) */
            if (sweep_count % 10 == 0) {
                printf("[watchdog_process] Sweep #%u — all processes %s\n",
                       sweep_count,
                       any_fault ? "FAULT DETECTED" : "OK");
            }
            continue;
        }

        if (code == PULSE_CODE_SHUTDOWN) {
            printf("[watchdog_process] Shutdown received.\n");
            break;
        }
    }

    timer_delete(tid);
    ConnectDetach(self_coid);
    name_detach(attach, 0);
    printf("[watchdog_process] Exiting.\n");
    return EXIT_SUCCESS;
}
