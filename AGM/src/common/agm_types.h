/*
 * agm_types.h  —  Shared data structures for AGM Real-Time Monitor
 *
 * All IPC messages, sensor readings, computed parameters, and alarm
 * definitions live here so every process works from the same layout.
 *
 * Rule: no process-local state in this file — pure data definitions only.
 */

#ifndef AGM_TYPES_H
#define AGM_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/neutrino.h>   /* struct _pulse, MsgSend/MsgReceive types */

/* =========================================================
 * 1.  RAW SENSOR DATA
 *     Produced by sensor_process every 20 ms.
 *     Sent to compute_process via MsgSend.
 * ========================================================= */
typedef struct {
    uint16_t    type;               /* MSG_TYPE_SENSOR_DATA — must be first */
    uint16_t    _pad;

    uint64_t    timestamp_ns;       /* CLOCK_MONOTONIC, nanoseconds          */
    uint32_t    seq_num;            /* monotonic counter, wrap-around OK     */

    /* --- Respiratory flow & pressure ------------------------------------ */
    float       flow_lpm;           /* instantaneous flow  [L/min]           */
    float       airway_pressure_cmH2O; /* instantaneous Paw [cmH2O]         */

    /* --- Gas concentrations --------------------------------------------- */
    float       fio2_pct;           /* fraction inspired O2        [%]       */
    float       fico2_mmhg;         /* inspired CO2 (rebreathing)  [mmHg]   */
    float       co2_waveform_mmhg;  /* instantaneous CO2 sample    [mmHg]   */
    float       fin2o_pct;          /* inspired N2O                [%]       */
    float       fiaa_pct;           /* inspired anesthetic agent   [%]       */

    /* --- Environment ---------------------------------------------------- */
    float       gas_temp_c;         /* breathing gas temperature   [°C]      */
    float       humidity_pct;       /* relative humidity           [%]       */
    float       atm_pressure_pa;    /* atmospheric pressure        [Pa]      */

    /* --- Fault / simulation flags --------------------------------------- */
    uint8_t     sensor_frozen;      /* 1 = data not advancing                */
    uint8_t     apnea_active;       /* 1 = apnea scenario injected           */
    uint8_t     _flags_pad[2];
} sensor_data_t;


/* =========================================================
 * 2.  COMPUTED PARAMETERS  (25 total — see Parameters Computed PDF)
 *     Written to shared memory by compute_process.
 *     Also sent as a message to logger_process.
 * ========================================================= */
typedef struct {
    uint16_t    type;               /* MSG_TYPE_COMPUTED_DATA                */
    uint16_t    _pad;

    uint64_t    timestamp_ns;
    uint32_t    breath_count;       /* total completed breaths               */

    /* --- Gas Monitoring (10) ------------------------------------------- */
    float       fio2_pct;           /* fraction inspired O2        [%]       */
    float       eto2_pct;           /* end-tidal O2                [%]       */
    float       fico2_mmhg;         /* fraction inspired CO2       [mmHg]   */
    float       etco2_mmhg;         /* end-tidal CO2               [mmHg]   */
    float       fin2o_pct;          /* fraction inspired N2O       [%]       */
    float       etn2o_pct;          /* end-tidal N2O               [%]       */
    float       fiaa_pct;           /* inspired anesthetic agent   [%]       */
    float       etaa_pct;           /* expired anesthetic agent    [%]       */
    float       mac;                /* minimum alveolar concentration        */
    float       mac_age;            /* age-adjusted MAC                      */

    /* --- Respiratory (4) ----------------------------------------------- */
    float       resp_rate_bpm;      /* respiratory rate    [breaths/min]     */
    float       tidal_vol_i_ml;     /* VTi — inspired tidal volume   [mL]   */
    float       tidal_vol_e_ml;     /* VTe — expired  tidal volume   [mL]   */
    float       minute_vent_lpm;    /* minute ventilation  [L/min]           */
    float       flow_rate_lpm;      /* instantaneous flow  [L/min]           */

    /* --- Pressure (4) -------------------------------------------------- */
    float       airway_pressure_cmH2O;
    float       pip_cmH2O;          /* peak inspiratory pressure   [cmH2O]  */
    float       peep_cmH2O;         /* PEEP                        [cmH2O]  */
    float       pmean_cmH2O;        /* mean airway pressure        [cmH2O]  */

    /* --- Volume (2) ---------------------------------------------------- */
    /* VTi and VTe listed above under Respiratory                           */

    /* --- Derived (2) --------------------------------------------------- */
    float       dynamic_compliance; /* Cdyn = VTi / (PIP - PEEP)  [mL/cmH2O] */
    float       static_compliance;  /* Cstat (simplified)          [mL/cmH2O] */

    /* --- Environment (3) ----------------------------------------------- */
    float       gas_temp_c;
    float       humidity_pct;
    float       atm_pressure_pa;

    /* --- Apnea tracking (safety use) ----------------------------------- */
    float       time_since_last_breath_s;
} computed_data_t;


/* =========================================================
 * 3.  ALARM DEFINITIONS
 *     Based on "List of Alarms" PDF, Sections 1–8.
 * ========================================================= */
typedef enum {
    /* Section 1 — Oxygen ------------------------------------------------ */
    ALARM_LOW_FIO2              = 0,  /* FiO2 < 21% for >5 s               */
    ALARM_HIGH_FIO2             = 1,  /* FiO2 > 60%                        */
    ALARM_RAPID_FIO2_DROP       = 2,  /* >10% drop within 5 s              */
    ALARM_FIO2_SENSOR_FAILURE   = 3,  /* no update >100 ms                 */
    ALARM_FIO2_OUT_OF_RANGE     = 4,  /* <0% or >100%                      */

    /* Section 2 — CO2 / Capnography ------------------------------------- */
    ALARM_HIGH_ETCO2            = 5,  /* EtCO2 > 50 mmHg for >3 s         */
    ALARM_LOW_ETCO2             = 6,  /* EtCO2 < 25 mmHg for >3 s         */
    ALARM_APNEA                 = 7,  /* no breath >10 s                   */
    ALARM_REBREATHING           = 8,  /* FiCO2 > 5 mmHg                   */
    ALARM_CO2_SENSOR_FAILURE    = 9,  /* no update >100 ms                 */
    ALARM_CO2_SATURATION        = 10, /* CO2 > 100 mmHg                   */

    /* Section 3 — N2O --------------------------------------------------- */
    ALARM_HIGH_N2O              = 11, /* N2O > 70%                         */
    ALARM_N2O_SENSOR_FAILURE    = 12,

    /* Section 4 — Anesthetic Agent -------------------------------------- */
    ALARM_HIGH_AA               = 13, /* > 2 × MAC                         */
    ALARM_LOW_AA                = 14, /* < 0.3 MAC                         */
    ALARM_RAPID_AA_CHANGE       = 15, /* >0.5% change in 1 s               */
    ALARM_AA_SENSOR_FAILURE     = 16,

    /* Section 5 — Ventilation / Respiratory ----------------------------- */
    ALARM_HIGH_RESP_RATE        = 17, /* > 30 breaths/min                  */
    ALARM_LOW_RESP_RATE         = 18, /* < 8 breaths/min                   */
    ALARM_LOW_TIDAL_VOL         = 19, /* VTe < 200 mL                      */
    ALARM_HIGH_TIDAL_VOL        = 20, /* VTe > 1000 mL                     */
    ALARM_LOW_MINUTE_VENT       = 21, /* < 3 L/min                         */
    ALARM_HIGH_MINUTE_VENT      = 22, /* > 12 L/min                        */
    ALARM_FLOW_SENSOR_FAILURE   = 23, /* no update >100 ms                 */

    /* Section 6 — Airway Pressure --------------------------------------- */
    ALARM_HIGH_PIP              = 24, /* PIP > 40 cmH2O                    */
    ALARM_LOW_AIRWAY_PRESSURE   = 25, /* Paw < 5 cmH2O                     */
    ALARM_SUSTAINED_HIGH_PRESS  = 26, /* > 35 cmH2O for >5 s              */
    ALARM_NEGATIVE_PRESSURE     = 27, /* < -2 cmH2O                        */
    ALARM_PRESSURE_SENSOR_FAIL  = 28,

    /* Section 7 — Gas Supply ------------------------------------------- */
    ALARM_O2_PIPELINE_LOW       = 29, /* < 300 kPa                         */
    ALARM_GAS_SUPPLY_LOSS       = 30, /* pressure = 0                      */

    /* Section 8 — Environment ------------------------------------------ */
    ALARM_HIGH_TEMP             = 31, /* > 50 °C                           */
    ALARM_LOW_TEMP              = 32, /* < 5 °C                            */
    ALARM_HIGH_HUMIDITY         = 33, /* > 90% RH                          */

    ALARM_COUNT                 = 34
} alarm_id_t;

typedef enum {
    ALARM_STATE_INACTIVE    = 0,
    ALARM_STATE_ACTIVE      = 1,
    ALARM_STATE_ACKNOWLEDGED = 2
} alarm_state_t;

typedef enum {
    ALARM_PRIORITY_LOW      = 0,
    ALARM_PRIORITY_MEDIUM   = 1,
    ALARM_PRIORITY_HIGH     = 2,
    ALARM_PRIORITY_CRITICAL = 3
} alarm_priority_t;

/* Per-alarm runtime state maintained by safety_process */
typedef struct {
    alarm_id_t          id;
    alarm_state_t       state;
    alarm_priority_t    priority;
    uint64_t            breach_start_ns;  /* when threshold was first crossed */
    uint64_t            last_trigger_ns;
    float               trigger_value;    /* value that caused the alarm      */
    char                description[80];
} alarm_t;

/* Static alarm metadata (thresholds, hysteresis) */
typedef struct {
    alarm_id_t          id;
    alarm_priority_t    priority;
    float               threshold;        /* primary threshold value          */
    float               duration_s;       /* 0 = instantaneous                */
    const char         *name;
} alarm_config_t;


/* =========================================================
 * 4.  SHARED MEMORY LAYOUT
 *     Mapped by compute_process (writer) and safety_process + GUI (readers).
 *     Name: /agm_params
 * ========================================================= */
typedef struct {
    pthread_mutex_t mutex;          /* PTHREAD_PROCESS_SHARED + prio-inherit */
    uint32_t        write_seq;      /* incremented before AND after write     */
    computed_data_t data;           /* latest computed parameters             */
    alarm_t         alarms[ALARM_COUNT]; /* alarm states for GUI future use  */
} shared_memory_t;


/* =========================================================
 * 5.  IPC MESSAGE WRAPPERS
 * ========================================================= */

/* Generic reply (used by all receivers) */
typedef struct {
    int32_t status;     /* 0 = OK, negative = error code */
} msg_reply_t;

/* Sensor → Compute: full sensor sample */
typedef sensor_data_t   msg_sensor_t;

/* Compute → Logger: full computed snapshot */
typedef computed_data_t msg_computed_t;

/* Union for MsgReceive buffer — must be large enough for any message */
typedef union {
    struct { uint16_t type; uint16_t subtype; } hdr;
    msg_sensor_t    sensor;
    msg_computed_t  computed;
    struct _pulse   pulse;          /* used when MsgReceive returns rcvid=0  */
    uint8_t         raw[512];       /* safety margin                         */
} agm_msg_t;

#endif /* AGM_TYPES_H */
