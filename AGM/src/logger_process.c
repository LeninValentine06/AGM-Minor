/*
 * logger_process.c  —  AGM Data Logger Process
 *
 * ROLE:    Application Layer (Layer 4) — background logging
 * PERIOD:  1 Hz (driven by compute_process which sends every 50th tick)
 * IPC IN:  MsgReceive(computed_data_t) ← compute_process
 * IPC OUT: MsgSendPulse(WD_FEED)       → watchdog_process
 *          stdout (formatted parameter table)
 *          (future: SD card / file write — mount point /data/agm.log)
 *
 * PRIORITY: Low — logger runs at the lowest priority of all AGM processes.
 *   In QNX, lower-priority processes are preempted by higher-priority ones.
 *   If the system is under load, logger may skip a log cycle — this is
 *   acceptable because logging is not safety-critical.
 *
 * VIVA NOTE — POSIX Message Queue alternative:
 *   We use MsgSend/MsgReceive here for consistency with the rest of the
 *   system.  An alternative is mq_open()/mq_send()/mq_receive() which
 *   provides a bounded queue (compute never blocks waiting for logger).
 *   The trade-off: message queues decouple timing more aggressively but
 *   consume kernel memory for queued messages.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <time.h>

#include "common/agm_types.h"
#include "common/ipc_common.h"

/* =========================================================
 * Formatted output helpers
 * ========================================================= */

/* Print a visual separator */
static void print_separator(void)
{
    printf("+-%-20s-+-%-10s-+-%-10s-+-%-10s-+\n",
           "--------------------", "----------",
           "----------", "----------");
}

static void print_param(const char *name, float val, const char *unit)
{
    printf("| %-20s | %10.3f | %-10s |\n", name, val, unit);
}

/*
 * log_snapshot() — print all 25 parameters in a structured table.
 *   This represents what would be written to the SD card in hardware.
 */
static void log_snapshot(const computed_data_t *cd, uint32_t log_seq)
{
    /* Wall-clock for log header */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  AGM LOG #%05u  t=%.3fs\n",
           log_seq,
           (double)cd->timestamp_ns * 1e-9);
    printf("╚══════════════════════════════════════════════╝\n");

    print_separator();
    printf("| %-20s | %-10s | %-10s |\n", "PARAMETER", "VALUE", "UNIT");
    print_separator();

    /* GAS MONITORING */
    printf("| %-20s |\n", "-- GAS MONITORING --");
    print_param("FiO2",          cd->fio2_pct,        "%");
    print_param("EtO2",          cd->eto2_pct,        "%");
    print_param("FiCO2",         cd->fico2_mmhg,      "mmHg");
    print_param("EtCO2",         cd->etco2_mmhg,      "mmHg");
    print_param("FiN2O",         cd->fin2o_pct,       "%");
    print_param("EtN2O",         cd->etn2o_pct,       "%");
    print_param("FiAA",          cd->fiaa_pct,        "%");
    print_param("EtAA",          cd->etaa_pct,        "%");
    print_param("MAC",           cd->mac,             "×MAC");
    print_param("MAC-Age",       cd->mac_age,         "×MAC");

    /* RESPIRATORY */
    printf("| %-20s |\n", "-- RESPIRATORY --");
    print_param("Resp Rate",     cd->resp_rate_bpm,   "bpm");
    print_param("VTi",           cd->tidal_vol_i_ml,  "mL");
    print_param("VTe",           cd->tidal_vol_e_ml,  "mL");
    print_param("Min Vent",      cd->minute_vent_lpm, "L/min");
    print_param("Flow Rate",     cd->flow_rate_lpm,   "L/min");

    /* PRESSURE */
    printf("| %-20s |\n", "-- PRESSURE --");
    print_param("Airway Press",  cd->airway_pressure_cmH2O, "cmH2O");
    print_param("PIP",           cd->pip_cmH2O,       "cmH2O");
    print_param("PEEP",          cd->peep_cmH2O,      "cmH2O");
    print_param("Pmean",         cd->pmean_cmH2O,     "cmH2O");

    /* DERIVED */
    printf("| %-20s |\n", "-- DERIVED --");
    print_param("Dyn Compliance", cd->dynamic_compliance, "mL/cmH2O");
    print_param("Stat Compliance",cd->static_compliance,  "mL/cmH2O");

    /* ENVIRONMENT */
    printf("| %-20s |\n", "-- ENVIRONMENT --");
    print_param("Gas Temp",      cd->gas_temp_c,      "°C");
    print_param("Humidity",      cd->humidity_pct,    "% RH");
    print_param("Atm Pressure",  cd->atm_pressure_pa, "Pa");

    print_separator();

    /* APNEA INDICATOR */
    if (cd->time_since_last_breath_s > 5.0f) {
        printf("| !! APNEA RISK: %.1fs since last breath |\n",
               cd->time_since_last_breath_s);
    }

    printf("| Breath count: %u  Apnea timer: %.1fs\n",
           cd->breath_count, cd->time_since_last_breath_s);
    printf("\n");
    fflush(stdout);
}

/* =========================================================
 * Main
 * ========================================================= */
int main(void)
{
    printf("[logger_process] PID %d starting\n", getpid());

    /* ---- Attach to name service --------------------------------------- */
    name_attach_t *attach = name_attach(NULL, AGM_LOGGER_CHANNEL, 0);
    if (!attach) {
        fprintf(stderr, "[logger_process] FATAL: name_attach failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    int chid = attach->chid;
    printf("[logger_process] Channel '%s' ready (chid=%d)\n",
           AGM_LOGGER_CHANNEL, chid);

    /* ---- Connect to watchdog ------------------------------------------ */
    int wd_coid = name_open_retry(AGM_WATCHDOG_CHANNEL, 10);

    agm_msg_t    msg;
    msg_reply_t  reply = { 0 };
    uint32_t     log_seq = 0;

    printf("[logger_process] Ready — waiting for log messages\n");

    /* ================================================================
     * MAIN RECEIVE LOOP — 1 Hz (driven by compute_process)
     * ================================================================ */
    for (;;) {
        struct _msg_info info;
        int rcvid = MsgReceive(chid, &msg, sizeof(msg), &info);

        if (rcvid == -1) {
            fprintf(stderr, "[logger_process] MsgReceive error: %s\n",
                    strerror(errno));
            continue;
        }

        if (rcvid == 0) {
            /* Pulse */
            if (msg.pulse.code == PULSE_CODE_SHUTDOWN) {
                printf("[logger_process] Shutdown received.\n");
                break;
            }
            continue;
        }

        /* Reply immediately — do not block compute_process */
        MsgReply(rcvid, 0, &reply, sizeof(reply));

        if (msg.hdr.type != MSG_TYPE_COMPUTED_DATA) {
            continue;
        }

        /* ---- Log to stdout (future: write to /data/agm.log on SD) ---- */
        log_snapshot(&msg.computed, ++log_seq);

        /* ---- Heartbeat to watchdog ------------------------------------ */
        if (wd_coid != -1) {
            MsgSendPulse(wd_coid, -1, PULSE_CODE_WD_FEED, WD_ID_LOGGER);
        }
    }

    if (wd_coid != -1) ConnectDetach(wd_coid);
    name_detach(attach, 0);
    printf("[logger_process] Exiting.\n");
    return EXIT_SUCCESS;
}
