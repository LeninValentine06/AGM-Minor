/*
 * AGM.c — Project stub (not compiled — see Makefile)
 *
 * The AGM system consists of 5 separate QNX processes.
 * Each has its own main() in a dedicated source file.
 * Build with: make
 * Run  with:  ./run_agm.sh
 *
 * Process map:
 *   sensor_process.c      — Layer 1: sensor simulation, 20 ms periodic
 *   compute_process.c     — Layer 2: 25-parameter computation, shared memory
 *   safety_process.c      — Layer 3: alarm evaluation, pulse-driven
 *   logger_process.c      — Layer 4: 1 Hz parameter logging
 *   watchdog_process.c    — Cross: liveness monitoring, 100 ms sweep
 */
