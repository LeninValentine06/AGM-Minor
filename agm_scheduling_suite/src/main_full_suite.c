/*
 * main_full_suite.c — AGM Full Experiment Suite Runner
 *
 * Runs all 13 To-Do items sequentially and prints results to stdout.
 * Designed for QNX Momentics console output.
 *
 * Build:
 *   qcc -V gcc_ntox86_64 -Wall -O1 -std=c99 -D_QNX_SOURCE \
 *       main_full_suite.c todo1_task_set.c todo2_todo3.c \
 *       todo4_single_thread.c todo5_multithread.c \
 *       todo6_7_policies.c todo8_interference.c \
 *       todo9_failures.c todo10_metrics.c todo13_conclusion.c \
 *       -lpthread -lm -o agm_full_suite
 *
 * Run:
 *   ./agm_full_suite           — run all 13 To-Dos
 *   ./agm_full_suite 4         — run only To-Do 4
 *   ./agm_full_suite 6 7 13    — run selected To-Dos
 *
 * NOTE: To-Do 11 (multi-process IPC) = your existing 5-process code.
 *       To-Do 12 (Qt GUI) = deferred (separate task).
 *       This suite covers 1-10 and 13 with measured data.
 */

#include "agm_common.h"
#include <time.h>

/* Forward declarations */
void run_todo1(void);
void run_todo2(void);
void run_todo3(void);
void run_todo4(void);
void run_todo5(void);
void run_todo6(void);
void run_todo7(void);
void run_todo8(void);
void run_todo9(void);
void run_todo10(void);
void run_todo13(void);

typedef void (*todo_fn_t)(void);

static const struct {
    int         num;
    const char *short_name;
    todo_fn_t   fn;
} TODOS[] = {
    { 1, "Task Set T",                 run_todo1  },
    { 2, "Criticality Classification", run_todo2  },
    { 3, "Thread Timing Groups",       run_todo3  },
    { 4, "Single-Thread Baselines",    run_todo4  },
    { 5, "Multi-Thread 1->n",          run_todo5  },
    { 6, "FIFO vs RR Policies",        run_todo6  },
    { 7, "Mixed Scheduling",           run_todo7  },
    { 8, "Interference Analysis",      run_todo8  },
    { 9, "Failure Scenarios",          run_todo9  },
    {10, "Performance Metrics",        run_todo10 },
    {13, "Optimal Strategy",           run_todo13 },
};
#define N_TODOS (int)(sizeof(TODOS)/sizeof(TODOS[0]))

static bool should_run(int num, int argc, char *argv[])
{
    if (argc <= 1) return true;   /* no args → run all */
    for (int i = 1; i < argc; i++) {
        if (atoi(argv[i]) == num) return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
    /* Print header */
    time_t now = time(NULL);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  AGM FULL EXPERIMENT SUITE — QNX Neutrino RTOS                         ║\n");
    printf("║  Deterministic Real-Time Anaesthesia Gas Monitor                       ║\n");
    printf("║  To-Do Items 1–10 + 13 (measured data for scheduling analysis)         ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  Run time : %-59s║\n", tbuf);
    printf("║  Platform : QNX VM (priority assignment requires root on real target)  ║\n");
    printf("║  Deadline : 20 ms hyperperiod                                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════╝\n");

    if (argc > 1) {
        printf("  Running selected To-Dos:");
        for (int i = 1; i < argc; i++) printf(" %s", argv[i]);
        printf("\n");
    } else {
        printf("  Running ALL To-Dos (1–10, 13). This takes ~60 seconds.\n");
        printf("  Tip: run ./agm_full_suite <num> to run a single To-Do.\n");
    }

    printf("\n  NOTE: To-Do 11 = your 5-process IPC code (separate build).\n");
    printf("        To-Do 12 = Qt GUI (deferred).\n\n");

    /* Index of To-Dos to run */
    printf("  CONTENTS:\n");
    for (int i = 0; i < N_TODOS; i++) {
        if (should_run(TODOS[i].num, argc, argv))
            printf("    [%2d] %s\n", TODOS[i].num, TODOS[i].short_name);
    }
    printf("\n");
    printf("  Press Enter to begin, or Ctrl+C to cancel...\n");
    getchar();

    /* Run selected To-Dos */
    double suite_start = now_ms();
    int ran = 0;

    for (int i = 0; i < N_TODOS; i++) {
        if (!should_run(TODOS[i].num, argc, argv)) continue;

        printf("\n>>> Starting To-Do %d: %s\n", TODOS[i].num, TODOS[i].short_name);
        fflush(stdout);

        double t0 = now_ms();
        TODOS[i].fn();
        double elapsed = now_ms() - t0;

        printf("\n  [To-Do %d complete — %.0f ms]\n", TODOS[i].num, elapsed);
        fflush(stdout);
        ran++;
    }

    double total = now_ms() - suite_start;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  SUITE COMPLETE — %2d To-Do items run in %.1f seconds               ║\n",
           ran, total / 1000.0);
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  Summary of coverage:                                                   ║\n");
    printf("║  To-Do 1  ✓  Formal task set T with utilisation analysis               ║\n");
    printf("║  To-Do 2  ✓  Tc (critical) and Tnc (non-critical) classification       ║\n");
    printf("║  To-Do 3  ✓  Thread timing groups — 12 tasks → 8 threads               ║\n");
    printf("║  To-Do 4  ✓  Single-thread baselines: min/avg/max exec, CPU%%           ║\n");
    printf("║  To-Do 5  ✓  1→8 thread scale: latency and deadline miss curves        ║\n");
    printf("║  To-Do 6  ✓  FIFO vs RR: latency, fairness, deadline performance       ║\n");
    printf("║  To-Do 7  ✓  Mixed scheduling: critical=FIFO, non-crit=RR/OTHER        ║\n");
    printf("║  To-Do 8  ✓  4 interference scenarios: GUI/log/compute/sensor overload ║\n");
    printf("║  To-Do 9  ✓  5 failure cases: normal/delay/fail/overload/starvation    ║\n");
    printf("║  To-Do 10 ✓  Full metrics: latency/CPU/memory/jitter/miss rate         ║\n");
    printf("║  To-Do 11 ─  5-process IPC code (separate build — already done)        ║\n");
    printf("║  To-Do 12 ─  Qt GUI (deferred)                                         ║\n");
    printf("║  To-Do 13 ✓  Optimal strategy: mixed FIFO+RR+OTHER, RM priorities      ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════╝\n\n");

    return 0;
}
