/*
 * agm_common.h — Shared definitions for AGM Full Experiment Suite
 *
 * _POSIX_C_SOURCE required for clock_gettime/nanosleep with -std=c99.
 * QNX defines this automatically; on Linux/GCC it must be explicit.
 * Must appear before any system #include.
 *
 * Priority table (from report Table 4):
 *   safety_thread          P=70  SCHED_FIFO  sporadic/event
 *   pressure_thread        P=65  SCHED_FIFO  20 ms
 *   flow_thread            P=64  SCHED_FIFO  20 ms
 *   waveform_thread        P=60  SCHED_FIFO  20 ms
 *   oxygen_thread          P=58  SCHED_FIFO  50 ms
 *   co2_thread             P=57  SCHED_FIFO  50 ms
 *   parameter_thread       P=55  SCHED_FIFO  50 ms
 *   agent_thread           P=50  SCHED_FIFO  100 ms
 *   environmental_thread   P=40  SCHED_FIFO  1000 ms
 *   watchdog_thread        P=30  SCHED_FIFO  1000 ms
 *   gui_thread             --    SCHED_OTHER 100 ms
 *   logging_thread         --    SCHED_OTHER 1000 ms
 */

#ifndef AGM_COMMON_H
#define AGM_COMMON_H

/* Must precede all system headers on Linux with -std=c99 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <sys/resource.h>

/* =========================================================
 * Priority assignments
 * ========================================================= */
#define PRIO_SAFETY          70
#define PRIO_PRESSURE        65
#define PRIO_FLOW            64
#define PRIO_WAVEFORM        60
#define PRIO_OXYGEN          58
#define PRIO_CO2             57
#define PRIO_PARAMETER       55
#define PRIO_AGENT           50
#define PRIO_ENVIRONMENTAL   40
#define PRIO_WATCHDOG        30

/* =========================================================
 * Nominal execution times (ms) — from report timing analysis
 * ========================================================= */
#define EXEC_PRESSURE_MS      2.0
#define EXEC_FLOW_MS          2.0
#define EXEC_WAVEFORM_MS      2.0
#define EXEC_OXYGEN_MS        3.0
#define EXEC_CO2_MS           3.0
#define EXEC_PARAMETER_MS     3.0
#define EXEC_AGENT_MS         3.0
#define EXEC_ENVIRONMENTAL_MS 1.0
#define EXEC_WATCHDOG_MS      1.0
#define EXEC_GUI_MS           3.0
#define EXEC_LOGGING_MS       2.0

/* Deadlines (ms) */
#define DEADLINE_FAST_MS      20.0
#define DEADLINE_MEDIUM_MS    50.0
#define DEADLINE_SLOW_MS     100.0
#define DEADLINE_BG_MS      1000.0

/* =========================================================
 * Thread identification
 * ========================================================= */
#define MAX_THREADS 16

typedef enum {
    TID_PRESSURE = 0,
    TID_FLOW,
    TID_WAVEFORM,
    TID_OXYGEN,
    TID_CO2,
    TID_PARAMETER,
    TID_AGENT,
    TID_ENVIRONMENTAL,
    TID_WATCHDOG,
    TID_GUI,
    TID_LOGGING,
    TID_SAFETY,
    TID_COUNT
} thread_id_t;

/* =========================================================
 * Per-thread measurement result
 * ========================================================= */
typedef struct {
    char        name[32];
    int         priority;
    const char *policy_str;
    int         period_ms;
    double      deadline_ms;
    bool        is_critical;
    double      start_ms;
    double      end_ms;
    double      exec_ms;
    bool        deadline_met;
    bool        starved;
    bool        preempted;
    /* For multi-run stats */
    double      exec_samples[10];
    int         sample_count;
    double      exec_min_ms;
    double      exec_max_ms;
    double      exec_avg_ms;
} thread_result_t;

/* =========================================================
 * Experiment result container
 * ========================================================= */
typedef struct {
    int             exp_num;
    const char     *exp_title;
    const char     *exp_desc;
    thread_result_t threads[MAX_THREADS];
    int             thread_count;
    double          pipeline_total_ms;
    int             deadline_misses;
    int             critical_misses;
    bool            watchdog_triggered;
    double          cpu_util_pct;
    const char     *conclusion;
} exp_result_t;

/* =========================================================
 * Timing helpers
 * ========================================================= */
static inline double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

static inline void busy_work_ms(double ms)
{
    double end = now_ms() + ms;
    volatile double x = 1.0;
    while (now_ms() < end)
        x = x * 1.0000001 + 0.0000001;
    (void)x;
}

static inline void sleep_ms(double ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)(fmod(ms, 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}

/* Apply scheduling policy — warns if no permission (runs anyway) */
static inline void set_sched(int policy, int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), policy, &sp) != 0) {
        /* Silently continue — timing still valid without elevated prio */
    }
}

/* =========================================================
 * Print helpers
 * ========================================================= */
static inline void banner(int num, const char *title, const char *desc)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  TO-DO %-3d — %-57s║\n", num, title);
    printf("║  %-71s║\n", desc);
    printf("╚══════════════════════════════════════════════════════════════════════════╝\n\n");
}

static inline void divider(void)
{
    printf("──────────────────────────────────────────────────────────────────────────\n");
}

static inline void section(const char *s)
{
    printf("\n  ▶ %s\n", s);
}

#endif /* AGM_COMMON_H */
