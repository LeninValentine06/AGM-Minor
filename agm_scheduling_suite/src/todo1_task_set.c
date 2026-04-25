/*
 * todo1_task_set.c — To-Do 1: Extract Formal Task Set T
 *
 * Identifies every task, computes utilisation U = Ci/Ti for each,
 * checks Liu & Layland schedulability bound (sum Ci/Ti <= n(2^1/n - 1)),
 * and prints the task dependency chain.
 */

#include "agm_common.h"

/* =========================================================
 * Formal task definition
 * ========================================================= */
typedef struct {
    const char *name;
    const char *function;
    int         period_ms;       /* Ti */
    const char *trigger;
    double      exec_ms;         /* Ci (WCET estimate) */
    double      deadline_ms;     /* Di — relative deadline */
    bool        is_critical;
} task_def_t;

static const task_def_t TASKS[] = {
    /* name                  function                       Ti      trigger    Ci    Di      crit */
    {"pressure_thread",  "Read airway pressure (HSC)",      20,  "periodic",  2.0,  20.0,  true },
    {"flow_thread",      "Read airflow (FS1015CL)",         20,  "periodic",  2.0,  20.0,  true },
    {"waveform_thread",  "Fill waveform buffer",            20,  "periodic",  2.0,  20.0,  true },
    {"oxygen_thread",    "Read O2 concentration (PSR-11)",  50,  "periodic",  3.0,  50.0,  true },
    {"co2_thread",       "Read CO2/capnography (SCD30)",    50,  "periodic",  3.0,  50.0,  true },
    {"parameter_thread", "Compute 25 clinical params",      50,  "periodic",  3.0,  50.0,  true },
    {"agent_thread",     "Read anaesthetic agent (IRMA)",  100,  "periodic",  3.0, 100.0,  true },
    {"safety_thread",    "Evaluate 34 alarms",               0,  "sporadic",  1.5,  50.0,  true },
    {"environmental_thd","Read temp/humidity/pressure",   1000,  "periodic",  1.0,1000.0,  false},
    {"watchdog_thread",  "Monitor all thread liveness",   1000,  "periodic",  1.0,1000.0,  false},
    {"gui_thread",       "Update Qt display (100 ms)",     100,  "periodic",  3.0, 100.0,  false},
    {"logging_thread",   "Write params to SD card",       1000,  "background",2.0,1000.0,  false},
};
#define N_TASKS (int)(sizeof(TASKS)/sizeof(TASKS[0]))

/* Dependency chain: task[i] depends on task[j] */
typedef struct { int from; int to; const char *ipc; } dep_t;
static const dep_t DEPS[] = {
    {0, 5, "MsgSend"},       /* pressure   → parameter  */
    {1, 5, "MsgSend"},       /* flow       → parameter  */
    {2, 5, "SharedMem"},     /* waveform   → parameter  */
    {3, 5, "MsgSend"},       /* oxygen     → parameter  */
    {4, 5, "MsgSend"},       /* co2        → parameter  */
    {6, 5, "MsgSend"},       /* agent      → parameter  */
    {5, 7, "Pulse+SHM"},     /* parameter  → safety     */
    {5,10, "SharedMem"},     /* parameter  → gui        */
    {5,11, "MsgSend"},       /* parameter  → logging    */
    {8, 5, "Monitor"},       /* watchdog   monitors all */
};
#define N_DEPS (int)(sizeof(DEPS)/sizeof(DEPS[0]))

void run_todo1(void)
{
    banner(1, "Extract Formal Task Set T",
           "Task name, function, period, trigger, WCET, deadline, utilisation");

    /* ---- Task Set Table ---- */
    section("TASK SET TABLE  (T = all tasks)");
    printf("  %-20s %-30s %6s %-10s %6s %8s %6s %5s\n",
           "Task","Function","Ti(ms)","Trigger","Ci(ms)","Di(ms)","Ui","Crit");
    divider();

    double total_util = 0.0;
    double critical_util = 0.0;
    int n_periodic = 0;

    for (int i = 0; i < N_TASKS; i++) {
        const task_def_t *t = &TASKS[i];
        double ui = (t->period_ms > 0) ? (t->exec_ms / (double)t->period_ms) : 0.0;
        total_util    += ui;
        if (t->is_critical) critical_util += ui;
        if (t->period_ms > 0) n_periodic++;

        printf("  %-20s %-30s %6d %-10s %6.1f %8.1f %6.4f %5s\n",
               t->name, t->function,
               t->period_ms, t->trigger,
               t->exec_ms, t->deadline_ms,
               ui, t->is_critical ? "✓ YES" : "NO");
    }

    /* ---- Schedulability Analysis ---- */
    section("SCHEDULABILITY ANALYSIS (Liu & Layland 1973)");

    /* LL bound: U <= n * (2^(1/n) - 1) for n periodic tasks */
    double ll_bound = (double)n_periodic * (pow(2.0, 1.0/(double)n_periodic) - 1.0);

    printf("  Total utilisation U       = %.4f  (%.1f%%)\n", total_util, total_util*100.0);
    printf("  Critical task util        = %.4f  (%.1f%%)\n", critical_util, critical_util*100.0);
    printf("  LL schedulability bound   = %.4f  (n=%d periodic tasks)\n", ll_bound, n_periodic);
    printf("  U <= LL bound?            = %s\n",
           total_util <= ll_bound ? "YES — schedulable under RM" :
           total_util <= 1.0      ? "LL missed but U<1 — may still schedule" :
                                    "NO — overloaded (U>1)");
    printf("  U <= 1.0 (necessary)?     = %s\n", total_util <= 1.0 ? "YES" : "NO — OVERLOADED");

    /* ---- Task Dependency Diagram ---- */
    section("TASK DEPENDENCY CHAIN (IPC mechanism shown)");
    printf("  Sensor threads → parameter_thread → safety_thread\n");
    printf("                                     → gui_thread\n");
    printf("                                     → logging_thread\n");
    printf("  watchdog_thread ←→ all threads (heartbeat pulse)\n\n");

    for (int i = 0; i < N_DEPS; i++) {
        printf("  %-20s  -[%-12s]→  %s\n",
               TASKS[DEPS[i].from].name,
               DEPS[i].ipc,
               TASKS[DEPS[i].to].name);
    }

    section("SUMMARY");
    printf("  Total tasks  : %d\n", N_TASKS);
    printf("  Critical     : %d (sensor + parameter + safety)\n",
           (int)(sizeof((int[]){0,1,2,3,4,5,6,7})/sizeof(int)));
    printf("  Non-critical : %d (environmental, watchdog, GUI, logging)\n", 4);
    printf("  Sporadic     : 1 (safety_thread — event-driven)\n");
}
