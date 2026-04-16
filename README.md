# Deterministic Real-Time Anaesthesia Gas Monitoring System
### QNX Neutrino RTOS | Multi-Process Architecture | Mixed-Criticality Scheduling

**Course:** 21IPE314P — RTOS Programming  
**Institution:** SRM Institute of Science and Technology, College of Engineering and Technology  
**Department:** Electronics and Communication Engineering  
**Team:** Lenin Valentine C J (RA2311004010073) · Arshad Ahmed B (RA2311004010083) · Harshith Kamal R (RA2311004010094)

---

## What This Project Does

This project implements a **deterministic real-time anaesthesia gas monitor (AGM)** running on **QNX Neutrino RTOS**. During surgical procedures, patients are placed under general anaesthesia and cannot breathe independently. A continuous, real-time monitoring system is required to track respiratory gases, anaesthetic agent concentrations, airway pressures, tidal volumes, and derived ventilation parameters — and to raise alarms immediately when any parameter crosses a safety threshold.

The system simulates what a real AGM hardware unit would do: acquire sensor data at 50 Hz, compute 25 clinical parameters in real time, evaluate 34 alarm conditions with sub-50 ms latency, log all parameters at 1 Hz, and monitor the liveness of all system processes via a dedicated hardware watchdog.

The key engineering challenge is **determinism** — the system must respond to critical conditions (like apnea) within a guaranteed time bound regardless of other system load. This is achieved through QNX's microkernel architecture, fixed-priority preemptive scheduling (SCHED_FIFO), and a mixed-criticality process design.

---

## Why QNX Neutrino RTOS?

Standard operating systems like Linux use general-purpose schedulers that do not guarantee worst-case execution times. In a medical device, a delayed alarm — even by a few hundred milliseconds — can be fatal.

QNX Neutrino provides:
- **Microkernel architecture** — each process runs in its own protected memory space. If one process crashes, the kernel terminates only that process; all others continue running.
- **Fixed-Priority Preemptive Scheduling (FPPS)** — higher-priority processes always preempt lower-priority ones, giving guaranteed response times.
- **POSIX-compliant IPC** — message passing (MsgSend/MsgReceive), shared memory, and pulse-based event notification with well-defined semantics.
- **Hard real-time timers** — nanosecond-resolution clocks (CLOCK_MONOTONIC) and absolute wake-up times (TIMER_ABSTIME) for drift-free periodic tasks.

---

## System Architecture

The system is structured as **5 independent QNX processes**, each in its own address space, communicating via QNX IPC:

```
sensor_process ──MsgSend──► compute_process ──SharedMem──► safety_process
                                    │                            │
                                    ├──MsgSend(1Hz)──► logger_process
                                    │
                                    └──WD_FEED pulses──► watchdog_process
                                                              ▲
                                         All processes feed WD heartbeat
```

### Process Roles

| Process | Layer | Scheduling | Priority | Period | Role |
|---------|-------|-----------|----------|--------|------|
| `sensor_process` | Layer 1 | SCHED_FIFO | High (~60) | 20 ms | Simulates 11 sensors, injects faults |
| `compute_process` | Layer 2 | SCHED_FIFO | P=55 | 20 ms | Computes all 25 clinical parameters |
| `safety_process` | Layer 3 | SCHED_FIFO | P=70 (highest) | Sporadic/Event | Evaluates 34 alarms — preempts compute on alarm |
| `logger_process` | Layer 4 | SCHED_OTHER | Low (non-critical) | 1000 ms | Logs parameter table to stdout / SD card |
| `watchdog_process` | Cross-cutting | SCHED_FIFO | P=30 | 100 ms sweep | Monitors all process heartbeats |

### Mixed-Criticality Design

The system explicitly separates **critical tasks (Tc)** from **non-critical tasks (Tnc)**:

- **Tc** (sensor, compute, safety, watchdog) — use `SCHED_FIFO` with fixed priorities. These processes must always meet their deadlines because a failure directly affects patient safety.
- **Tnc** (logger) — uses `SCHED_OTHER`. Logging can be delayed or preempted without clinical consequence.

This is a **mixed-criticality system** where critical tasks are guaranteed CPU time and non-critical tasks run only when the CPU is otherwise idle.

---

## Parameters Monitored (25 Total)

| Category | Parameters |
|----------|-----------|
| Gas Monitoring (10) | FiO₂, EtO₂, FiCO₂, EtCO₂, FiN₂O, EtN₂O, FiAA, EtAA, MAC, MAC-Age |
| Respiratory (5) | Respiratory Rate, VTi, VTe, Minute Ventilation, Flow Rate |
| Pressure (4) | Airway Pressure, PIP, PEEP, Pmean |
| Derived (2) | Dynamic Compliance (Cdyn), Static Compliance (Cstat) |
| Environment (3) | Gas Temperature, Humidity, Atmospheric Pressure |
| Apnea Tracking (1) | Time since last detected breath |

---

## Alarm System (34 Alarms)

Alarms are evaluated by `safety_process` on every `PARAMS_READY` pulse from compute. Each alarm has:
- A **threshold** value
- A **duration** requirement (e.g. apnea fires only after 10 s of no breath — prevents spurious alarms from transient noise)
- A **priority level**: LOW / MEDIUM / HIGH / CRITICAL

| Section | Alarms |
|---------|--------|
| Section 1 — Oxygen | Low FiO₂, High FiO₂, Rapid drop, Sensor failure, Out-of-range |
| Section 2 — CO₂/Capnography | High/Low EtCO₂, **APNEA [CRITICAL]**, Rebreathing, CO₂ saturation, Sensor failure |
| Section 3 — N₂O | High N₂O, Sensor failure |
| Section 4 — Anaesthetic Agent | High AA (>2×MAC) [CRITICAL], Low AA (<0.3 MAC), Rapid change, Sensor failure |
| Section 5 — Ventilation | High/Low RR, Low/High tidal volume, Low/High minute ventilation, Flow sensor failure |
| Section 6 — Airway Pressure | High PIP, Low airway pressure, Sustained high pressure, Negative pressure, Sensor failure |
| Section 7 — Gas Supply | O₂ pipeline low, Gas supply loss |
| Section 8 — Environment | High/Low temperature, High humidity |

---

## IPC Mechanisms Used

| Mechanism | Path | Why |
|-----------|------|-----|
| `MsgSend / MsgReceive` | sensor → compute | Synchronous, blocking. Provides natural flow control and priority inheritance. |
| `POSIX Shared Memory` | compute → safety | Lock-free seqlock reads. Allows safety to read parameters without blocking compute. |
| `MsgSendPulse (PARAMS_READY)` | compute → safety | Non-blocking 8-byte event. compute continues its 20 ms loop immediately after notifying safety. |
| `MsgSendPulse (WD_FEED)` | all → watchdog | Heartbeat every period. Watchdog detects hung processes within 200 ms. |
| `MsgSend (1 Hz)` | compute → logger | Full 25-parameter snapshot sent every 50 ticks for background logging. |
| `QNX POSIX Timer` | kernel → watchdog | 100 ms pulse-driven sweep — timers delivered as pulses, not signals, per QNX real-time pattern. |

---

## Simulation Engine

Since physical sensors (SCD30, FS1015, HSC, PSR-11, etc.) are not available in the current development phase, `sim_engine.c` provides a **deterministic mathematical waveform generator** that replaces them.

All waveforms are computed from `tick × period_ms` — no `rand()` calls. The same tick always produces the same output, enabling regression testing.

Waveforms modelled:
- **Flow**: Half-sine approximation with I:E ratio 1:1.5 (peak 30 L/min)
- **Capnography CO₂**: Piecewise 4-phase model (dead-space → rapid rise → alveolar plateau → inspiratory washout)
- **Airway Pressure**: Raised cosine ramp during inspiration, exponential decay during expiration
- **FiO₂**: Slow sinusoidal drift ±2% over 60 s (exercises alarm thresholds)
- **Anaesthetic Agent**: Very slow rise to test RAPID_AA_CHANGE alarm

### Fault Injection Schedule

The demo runs a timed fault injection sequence to validate the alarm system:

| Time | Scenario | What to Observe |
|------|----------|-----------------|
| t = 0–30 s | Normal operation | All parameters in range, watchdog OK |
| t = 30–60 s | **APNEA injected** | Flow = 0, CO₂ flatlines. ALARM_APNEA fires at t≈40 s |
| t = 60–90 s | **SENSOR FREEZE** | seq_num stops advancing. Watchdog FAULT within 200 ms |
| t = 90+ s | Recovery | Alarms clear, watchdog RECOVERED, normal operation resumes |

---

## Project Structure

```
AGM/
├── Makefile                    ← Build system (qcc, x86_64-debug target)
├── run_agm.sh                  ← Launch script — starts all 5 processes in order
└── src/
    ├── common/
    │   ├── agm_types.h         ← All shared data structures (sensor_data_t,
    │   │                          computed_data_t, alarm_t, shared_memory_t)
    │   └── ipc_common.h        ← Channel names, pulse codes, timing constants,
    │                              helper functions (now_ns, name_open_retry)
    ├── sensor/
    │   ├── sim_engine.h        ← Simulation engine interface
    │   └── sim_engine.c        ← Deterministic waveform generator
    ├── sensor_process.c        ← Layer 1: 20 ms periodic sensor acquisition
    ├── compute_process.c       ← Layer 2: 25-parameter computation + shared memory
    ├── safety_process.c        ← Layer 3: 34-alarm evaluation, pulse-driven
    ├── logger_process.c        ← Layer 4: 1 Hz background data logging
    └── watchdog_process.c      ← Cross: process liveness monitoring
```

---

## Build and Run

### Prerequisites
- QNX Neutrino RTOS 8.0 (x86_64 VM or hardware)
- QNX Momentics IDE with QNX SDP 8.0
- `qcc` compiler (GCC backend for QNX)
- `qconn` running on the QNX target (port 8000)

### Build

```bash
# In QNX Momentics IDE:
# Project → Build All  (uses Makefile automatically)

# Or from QNX terminal:
make clean && make
# Produces: build/x86_64-debug/{sensor,compute,safety,logger,watchdog}_process
```

### Run

Processes must start in dependency order. Use the provided script:

```bash
chmod +x run_agm.sh
./run_agm.sh
```

Or launch manually (with delays between each):

```bash
rm -f /dev/shmem/agm_params
./watchdog_process &  ; sleep 0.5
./compute_process  &  ; sleep 0.5
./logger_process   &  ; sleep 0.3
./safety_process   &  ; sleep 0.3
./sensor_process   &
```

**Launch order matters** — watchdog and compute must be running before safety tries to map shared memory.

### Capture Output to Log Files

```bash
rm -f /dev/shmem/agm_params
./watchdog_process > log_watchdog.txt 2>&1 &  ; sleep 1
./compute_process  > log_compute.txt  2>&1 &  ; sleep 1
./logger_process   > log_logger.txt   2>&1 &  ; sleep 0.5
./safety_process   > log_safety.txt   2>&1 &  ; sleep 0.5
./sensor_process   > log_sensor.txt   2>&1 &
sleep 100
slay sensor_process logger_process safety_process compute_process watchdog_process
```

---

## Key Design Decisions and Viva Notes

**Why separate processes instead of threads?**  
If `sensor_process` crashes, the QNX microkernel terminates only that process. `compute_process` and `safety_process` continue running, and `watchdog_process` detects the missing heartbeat within 200 ms. With threads, a single runaway thread can corrupt the entire address space.

**Why MsgSend (blocking) from sensor to compute?**  
`MsgSend` blocks `sensor_process` until `compute_process` replies. This provides natural back-pressure (flow control) — sensor cannot produce data faster than compute can consume it. QNX priority inheritance ensures compute temporarily inherits sensor's priority if preempted while holding the reply, preventing priority inversion.

**Why a pulse (non-blocking) from compute to safety?**  
`MsgSendPulse` is non-blocking (8 bytes, kernel-level). compute delivers `PARAMS_READY` and immediately continues its 20 ms loop. Safety reads shared memory at its own pace. If safety is slow for one cycle, the next pulse overwrites the pending one — safety always evaluates the most recent data, never stale data.

**Why shared memory for safety instead of MsgSend?**  
Shared memory allows safety to read `computed_data_t` without blocking compute. A `PTHREAD_PROCESS_SHARED` mutex with `PTHREAD_PRIO_INHERIT` protects writes; a seqlock pattern (`write_seq` counter) allows lock-free reads by safety, preventing priority inversion on the read path.

**Why TIMER_ABSTIME for sensor timing?**  
`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wake, NULL)` sleeps until an absolute time. Any processing jitter inside the loop is automatically absorbed. Using `nanosleep(20ms)` would accumulate drift — each sleep adds processing time, causing the sensor to run slower than 50 Hz over time.

**Why deterministic noise instead of rand()?**  
Medical device sensor simulation must be repeatable for regression testing. A small-amplitude sinusoid at 7.3 Hz (co-prime to the 15 BPM breath cycle) provides reproducible "noise" — the same tick always produces the same value, so test suites can compare exact output values.

---

## Hardware Target (Planned)

| Component | Model | Interface | Parameters |
|-----------|-------|-----------|-----------|
| Central Processor | Raspberry Pi 4, 4 GB | — | Runs QNX Neutrino RTOS |
| O₂ Sensor | PSR-11-39-MD | ADC (ADS1115) | FiO₂, EtO₂ |
| CO₂ Sensor | SCD30 | I²C | EtCO₂, FiCO₂ |
| N₂O Sensor | NDIR | ADC | FiN₂O, EtN₂O |
| Agent Sensor | IRMA | UART | FiAA, EtAA, MAC |
| Pressure | HSC | I²C | PIP, PEEP, Pmean |
| Flow | FS1015CL-150 | ADC | Flow, VT, RR, MV |
| Temperature | PT100 | I²C | Gas temp compensation |
| Humidity | SHT31 | I²C | Humidity compensation |
| Barometric | BMP388 | I²C | Atmospheric pressure |
| Display | 7" HDMI TFT Touchscreen | HDMI | Real-time parameter GUI |
| Alarm Output | Buzzer + RGB LED | GPIO | Audible and visual alarms |

In the current implementation, `sim_engine.c` replaces all physical sensors with mathematically modelled waveforms.

---

## Current Status

- [x] 5-process QNX multi-process architecture — implemented and running
- [x] 25-parameter computation from simulated sensor data
- [x] 34 alarms with threshold, duration, and hysteresis logic
- [x] Mixed-criticality scheduling (SCHED_FIFO critical + SCHED_OTHER non-critical)
- [x] Fault injection: APNEA, SENSOR_FREEZE, RECOVERY
- [x] Watchdog process with 100 ms QNX timer pulse sweep
- [x] POSIX shared memory with process-shared mutex and seqlock
- [x] Drift-free 20 ms sensor timing via TIMER_ABSTIME
- [ ] Qt 6 GUI — real-time waveform display (planned for Review 3)
- [ ] SD card / file logging replacing stdout (planned)
- [ ] Physical hardware sensor integration (planned)
- [ ] Formal WCET measurement using QNX System Profiler (planned)

---

## References

1. S. C. Meka, S. Achan, R. G. Pettit — "Real-Time Embedded Monitoring Technologies in Modern Healthcare Systems" — IEEE INES 2024
2. D. F. T. Morais et al. — "IoT-Based Wearable and Smart Health Device Solutions for Capnography" — Electronics (MDPI) 2023
3. M. Ali et al. — "Fault-Tolerant Medical Monitoring Systems" — IEEE EMBS 2020
4. K. Evans et al. — "Microkernel Architecture in Medical Devices" — Springer Embedded Systems Journal 2022
5. P. Thomas et al. — "Safety-Critical Scheduling in RTOS" — ACM RTSS 2021
6. QNX Neutrino RTOS System Architecture Guide — BlackBerry QNX 2024
