/*
 * todo2_criticality.c — To-Do 2: Classify Critical vs Non-Critical Tasks
 * todo3_thread_groups.c — To-Do 3: Redesign Threads Based on Timing Groups
 *
 * Defines Tc (critical) and Tnc (non-critical) sets,
 * explains classification reasoning, then shows thread grouping.
 */

#include "agm_common.h"

/* =========================================================
 * TO-DO 2
 * ========================================================= */
void run_todo2(void)
{
    banner(2, "Classify Critical vs Non-Critical Tasks",
           "Tc = safety-affecting tasks; Tnc = background/display tasks");

    typedef struct {
        const char *name;
        const char *criticality;
        const char *reasoning;
    } class_row_t;

    static const class_row_t rows[] = {
        {"pressure_thread",  "CRITICAL",     "Airway pressure loss → barotrauma in <2 s; must detect instantly"},
        {"flow_thread",      "CRITICAL",     "Zero flow = no ventilation; apnea detected from flow signal"},
        {"waveform_thread",  "CRITICAL",     "Capnography waveform is primary CO2 signal; feeds safety alarms"},
        {"oxygen_thread",    "CRITICAL",     "FiO2 < 21% = hypoxia risk; must alarm within 5 s per IEC 60601"},
        {"co2_thread",       "CRITICAL",     "EtCO2 thresholds determine hypo/hypercapnia alarms"},
        {"parameter_thread", "CRITICAL",     "Derives all 25 clinical params — upstream of every alarm"},
        {"agent_thread",     "CRITICAL",     "MAC overdose (>2x MAC) is immediately life-threatening"},
        {"safety_thread",    "CRITICAL",     "Evaluates all 34 alarms — direct patient safety action"},
        {"environmental_thd","NON-CRITICAL", "Temp/humidity compensation — slow-changing, 1 s tolerance"},
        {"watchdog_thread",  "NON-CRITICAL", "System health monitor — not in patient signal path"},
        {"gui_thread",       "NON-CRITICAL", "Display update — clinician reads display; 100 ms lag acceptable"},
        {"logging_thread",   "NON-CRITICAL", "SD card write — retrospective record; not real-time safety"},
    };
    int n = (int)(sizeof(rows)/sizeof(rows[0]));

    section("CRITICALITY CLASSIFICATION TABLE");
    printf("  %-20s %-14s  %s\n", "Task", "Criticality", "Reasoning");
    divider();
    for (int i = 0; i < n; i++) {
        printf("  %-20s %-14s  %s\n",
               rows[i].name, rows[i].criticality, rows[i].reasoning);
    }

    section("FORMAL SETS");
    printf("  T  = { all 12 threads }\n\n");
    printf("  Tc = { pressure, flow, waveform, oxygen, co2,\n");
    printf("         parameter, agent, safety }\n\n");
    printf("  Tnc = { environmental, watchdog, gui, logging }\n\n");

    section("CLASSIFICATION CRITERIA");
    printf("  A task is CRITICAL if any of the following apply:\n");
    printf("    1. Its output feeds a patient-safety alarm directly.\n");
    printf("    2. A missed deadline can cause delayed or missed alarm.\n");
    printf("    3. It is in the sensor-to-alarm signal path.\n\n");
    printf("  A task is NON-CRITICAL if:\n");
    printf("    1. Its delay does not affect alarm generation timing.\n");
    printf("    2. It is for display, logging, or system diagnostics.\n");
    printf("    3. IEC 60601-2-13 does not mandate its response time.\n");

    section("SCHEDULING IMPLICATION");
    printf("  Critical tasks → SCHED_FIFO (deterministic, preemptive)\n");
    printf("  Non-critical   → SCHED_OTHER (best-effort, no starvation of FIFO tasks)\n");
}

/* =========================================================
 * TO-DO 3
 * ========================================================= */
void run_todo3(void)
{
    banner(3, "Redesign Threads Based on Timing Groups",
           "Group tasks by period; fewer threads, lower context switching");

    typedef struct {
        const char *thread_name;
        int         period_ms;
        int         priority;
        const char *policy;
        const char *tasks_inside;
        const char *advantage;
    } group_t;

    static const group_t groups[] = {
        {
            "fast_sensor_thread",   20,  64, "SCHED_FIFO",
            "pressure + flow + waveform_buffer",
            "Single 20ms thread handles all I2C fast sensors; eliminates 2 context switches per cycle"
        },
        {
            "medium_sensor_thread", 50,  57, "SCHED_FIFO",
            "oxygen + CO2 + agent",
            "SCD30/PSR-11/IRMA share 50ms period; batching reduces scheduling overhead"
        },
        {
            "parameter_thread",     50,  55, "SCHED_FIFO",
            "compute all 25 clinical parameters",
            "Dedicated compute thread; runs immediately after medium_sensor_thread releases"
        },
        {
            "safety_thread",         0,  70, "SCHED_FIFO",
            "evaluate all 34 alarms (sporadic)",
            "Highest priority; preempts everything on PARAMS_READY pulse; <1.5ms latency"
        },
        {
            "gui_thread",          100,   0, "SCHED_OTHER",
            "Qt parameter display + waveform update",
            "Non-critical; SCHED_OTHER prevents it from blocking critical FIFO threads"
        },
        {
            "logging_thread",     1000,   0, "SCHED_OTHER",
            "write 25 params to SD card with RTC timestamp",
            "Background; SD write latency (1-10ms) absorbed by SCHED_OTHER scheduler"
        },
        {
            "environmental_thread",1000, 40, "SCHED_FIFO",
            "read temp + humidity + barometric pressure",
            "Low FIFO priority (P=40); slow sensors; runs only when no higher-prio work exists"
        },
        {
            "watchdog_thread",    1000,  30, "SCHED_FIFO",
            "check all thread heartbeats; feed HW WDT",
            "Must run at least once per 50ms window; lowest FIFO priority is sufficient"
        },
    };
    int n = (int)(sizeof(groups)/sizeof(groups[0]));

    section("THREAD ARCHITECTURE TABLE");
    printf("  %-26s %6s %6s %-12s\n", "Thread", "Period", "Prio", "Policy");
    printf("  %-26s %-30s\n", "", "Tasks inside");
    divider();

    for (int i = 0; i < n; i++) {
        printf("  %-26s %5dms %6d %-12s\n",
               groups[i].thread_name,
               groups[i].period_ms,
               groups[i].priority,
               groups[i].policy);
        printf("    Tasks: %-55s\n", groups[i].tasks_inside);
        printf("    Benefit: %s\n\n", groups[i].advantage);
    }

    section("ORIGINAL vs REDESIGNED — THREAD COUNT");
    printf("  Original (one thread per sensor):  11 threads\n");
    printf("  Redesigned (timing groups)      :   8 threads\n");
    printf("  Reduction                        :   3 fewer context switches per 20ms cycle\n\n");

    section("TASK → THREAD MAPPING TABLE");
    printf("  %-24s → %-28s %s\n", "Task","Thread","Period");
    divider();
    static const struct { const char *task; const char *thread; const char *period; } map[] = {
        {"pressure_sensor",   "fast_sensor_thread",    "20 ms"},
        {"flow_sensor",       "fast_sensor_thread",    "20 ms"},
        {"waveform_buffer",   "fast_sensor_thread",    "20 ms"},
        {"oxygen_sensor",     "medium_sensor_thread",  "50 ms"},
        {"co2_sensor",        "medium_sensor_thread",  "50 ms"},
        {"agent_sensor",      "medium_sensor_thread",  "50 ms"},
        {"parameter_compute", "parameter_thread",      "50 ms"},
        {"alarm_evaluation",  "safety_thread",         "event"},
        {"display_update",    "gui_thread",            "100 ms"},
        {"data_logging",      "logging_thread",        "1000 ms"},
        {"env_compensation",  "environmental_thread",  "1000 ms"},
        {"heartbeat_check",   "watchdog_thread",       "1000 ms"},
    };
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        printf("  %-24s → %-28s %s\n", map[i].task, map[i].thread, map[i].period);
    }
}
