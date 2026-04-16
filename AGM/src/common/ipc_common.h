/*
 * ipc_common.h  —  IPC constants shared across all AGM processes
 *
 * Channel names, shared memory name, message type codes, pulse codes,
 * timing constants, and watchdog process IDs.
 *
 * Design rationale (viva note):
 *   Centralising these constants ensures no process hard-codes a channel
 *   name.  If a channel is renamed, one edit here fixes all processes.
 */

#ifndef IPC_COMMON_H
#define IPC_COMMON_H

#include <stdint.h>

/* =========================================================
 * QNX Name Service — channel names
 *   name_attach(NULL, NAME, 0)  →  server side
 *   name_open(NAME, 0)          →  client side (returns coid)
 * ========================================================= */
#define AGM_COMPUTE_CHANNEL     "agm_compute"
#define AGM_SAFETY_CHANNEL      "agm_safety"
#define AGM_LOGGER_CHANNEL      "agm_logger"
#define AGM_WATCHDOG_CHANNEL    "agm_watchdog"

/* =========================================================
 * Shared Memory
 *   shm_open("/agm_params", O_RDWR|O_CREAT, 0666)
 * ========================================================= */
#define AGM_SHM_NAME            "/agm_params"

/* =========================================================
 * Message Types  (uint16_t — first field of every message)
 * ========================================================= */
#define MSG_TYPE_SENSOR_DATA    0x0001  /* sensor_process  → compute_process */
#define MSG_TYPE_COMPUTED_DATA  0x0002  /* compute_process → logger_process  */
#define MSG_TYPE_SHUTDOWN       0x00FF  /* any process     → any process     */

/* =========================================================
 * Pulse Codes  (sent via MsgSendPulse)
 *   _PULSE_CODE_MINAVAIL is the first user-available code in QNX.
 *   Pulses are non-blocking — ideal for event signalling.
 * ========================================================= */
#include <sys/neutrino.h>   /* pulse codes, MsgSendPulse               */
#include <sys/dispatch.h>   /* name_attach, name_open, name_detach     */

#define PULSE_CODE_PARAMS_READY  (_PULSE_CODE_MINAVAIL + 0)
    /* compute → safety: new computed_data is in shared memory */

#define PULSE_CODE_ALARM_ACTIVE  (_PULSE_CODE_MINAVAIL + 1)
    /* safety → GUI (future): an alarm state changed */

#define PULSE_CODE_WD_FEED       (_PULSE_CODE_MINAVAIL + 2)
    /* any process → watchdog: "I am alive" heartbeat */

#define PULSE_CODE_SHUTDOWN      (_PULSE_CODE_MINAVAIL + 3)
    /* watchdog → all: ordered shutdown */

/* =========================================================
 * Watchdog process IDs  (sent as pulse value in WD_FEED pulse)
 * ========================================================= */
typedef enum {
    WD_ID_SENSOR    = 0,
    WD_ID_COMPUTE   = 1,
    WD_ID_SAFETY    = 2,
    WD_ID_LOGGER    = 3,
    WD_ID_COUNT     = 4
} wd_process_id_t;

/* =========================================================
 * Timing Constants
 * ========================================================= */
#define SENSOR_PERIOD_NS        20000000LL   /*  20 ms — sensor loop         */
#define COMPUTE_PERIOD_NS       50000000LL   /*  50 ms — used for logging    */
#define LOGGER_PERIOD_NS        1000000000LL /* 1000 ms — log interval       */
#define WD_TIMEOUT_NS           200000000LL  /* 200 ms — missed feed limit   */
#define WD_CHECK_PERIOD_NS      100000000LL  /* 100 ms — watchdog poll rate  */

/* How many missed feeds before declaring a process dead */
#define WD_MAX_MISSED_FEEDS     3

/* =========================================================
 * Helper: add nanoseconds to a timespec (avoids overflow)
 * ========================================================= */
static inline void timespec_add_ns(struct timespec *ts, long long ns)
{
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000LL) {
        ts->tv_nsec -= 1000000000LL;
        ts->tv_sec++;
    }
}

/* Helper: delta in nanoseconds between two timespecs (a - b) */
static inline long long timespec_diff_ns(const struct timespec *a,
                                          const struct timespec *b)
{
    return (long long)(a->tv_sec  - b->tv_sec)  * 1000000000LL
         + (long long)(a->tv_nsec - b->tv_nsec);
}

/* Helper: current CLOCK_MONOTONIC time in nanoseconds */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Helper: open a named channel with retry (processes may start in any order) */
#include <unistd.h>
static inline int name_open_retry(const char *name, int max_tries)
{
    int coid;
    int tries = 0;
    struct timespec wait = { 0, 100000000L }; /* 100 ms */
    while ((coid = name_open(name, 0)) == -1 && tries < max_tries) {
        nanosleep(&wait, NULL);
        tries++;
    }
    return coid;  /* -1 if all retries failed */
}

#endif /* IPC_COMMON_H */
