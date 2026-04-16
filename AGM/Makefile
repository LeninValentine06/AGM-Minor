# =============================================================================
# AGM Real-Time Monitor — QNX Neutrino Build System
#
# Builds 5 separate process binaries (microkernel design: one process per role)
#   sensor_process    — sensor simulation + fault injection   (Layer 1)
#   compute_process   — 25-parameter computation + shared mem (Layer 2)
#   safety_process    — alarm evaluation, pulse-driven        (Layer 3)
#   logger_process    — background 1 Hz parameter logging     (Layer 4)
#   watchdog_process  — cross-process liveness monitoring
#
# Usage:
#   make                         debug build (x86_64)
#   make BUILD_PROFILE=release   release build
#   make clean                   remove build artefacts
#   make rebuild                 clean then all
# =============================================================================

# ---------------------------------------------------------------------------
# Target / toolchain
# ---------------------------------------------------------------------------
PLATFORM      ?= x86_64
BUILD_PROFILE ?= debug
CONFIG_NAME    = $(PLATFORM)-$(BUILD_PROFILE)
OUTPUT_DIR     = build/$(CONFIG_NAME)

CC  = qcc -Vgcc_nto$(PLATFORM)
LD  = $(CC)

# ---------------------------------------------------------------------------
# Include paths  (covers src/, src/common/, src/sensor/)
# ---------------------------------------------------------------------------
INCLUDES = -I src -I src/common -I src/sensor

# ---------------------------------------------------------------------------
# Compiler flags per profile
# ---------------------------------------------------------------------------
CCFLAGS_debug    = -g -O0 -fno-builtin
CCFLAGS_release  = -O2 -DNDEBUG
CCFLAGS_coverage = -g -O0 -ftest-coverage -fprofile-arcs
CCFLAGS_profile  = -g -O0 -finstrument-functions

CCFLAGS = -Wall -Wextra -fmessage-length=0 $(INCLUDES) $(CCFLAGS_$(BUILD_PROFILE))

# ---------------------------------------------------------------------------
# Linker flags & libraries
# ---------------------------------------------------------------------------
LDFLAGS_coverage = -ftest-coverage -fprofile-arcs
LDFLAGS  = $(LDFLAGS_$(BUILD_PROFILE))
LIBS     = -lm

# ---------------------------------------------------------------------------
# Output directories that must exist before any .o is built
# (Using order-only prerequisites prevents spurious rebuilds)
# ---------------------------------------------------------------------------
OBJ_DIRS = \
    $(OUTPUT_DIR)/src          \
    $(OUTPUT_DIR)/src/sensor

$(OBJ_DIRS):
	mkdir -p $@

# ---------------------------------------------------------------------------
# Single, non-duplicate pattern rule
#   Depends on OBJ_DIRS (order-only) so directories always exist first,
#   which is critical for parallel make (-j).
# ---------------------------------------------------------------------------
$(OUTPUT_DIR)/%.o: %.c | $(OBJ_DIRS)
	@echo "  [CC] $<"
	$(CC) -c -MMD -MP -o $@ $(CCFLAGS) $<

# ---------------------------------------------------------------------------
# Per-binary source → object → binary rules
# ---------------------------------------------------------------------------

# --- sensor_process  (only binary that needs sim_engine.c) ----------------
SENSOR_SRCS = src/sensor_process.c  src/sensor/sim_engine.c
SENSOR_OBJS = $(patsubst %.c,$(OUTPUT_DIR)/%.o,$(SENSOR_SRCS))

$(OUTPUT_DIR)/sensor_process: $(SENSOR_OBJS)
	@echo "  [LD] $@"
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

# --- compute_process ------------------------------------------------------
COMPUTE_SRCS = src/compute_process.c
COMPUTE_OBJS = $(patsubst %.c,$(OUTPUT_DIR)/%.o,$(COMPUTE_SRCS))

$(OUTPUT_DIR)/compute_process: $(COMPUTE_OBJS)
	@echo "  [LD] $@"
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

# --- safety_process -------------------------------------------------------
SAFETY_SRCS = src/safety_process.c
SAFETY_OBJS = $(patsubst %.c,$(OUTPUT_DIR)/%.o,$(SAFETY_SRCS))

$(OUTPUT_DIR)/safety_process: $(SAFETY_OBJS)
	@echo "  [LD] $@"
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

# --- logger_process -------------------------------------------------------
LOGGER_SRCS = src/logger_process.c
LOGGER_OBJS = $(patsubst %.c,$(OUTPUT_DIR)/%.o,$(LOGGER_SRCS))

$(OUTPUT_DIR)/logger_process: $(LOGGER_OBJS)
	@echo "  [LD] $@"
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

# --- watchdog_process -----------------------------------------------------
WATCHDOG_SRCS = src/watchdog_process.c
WATCHDOG_OBJS = $(patsubst %.c,$(OUTPUT_DIR)/%.o,$(WATCHDOG_SRCS))

$(OUTPUT_DIR)/watchdog_process: $(WATCHDOG_OBJS)
	@echo "  [LD] $@"
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

# ---------------------------------------------------------------------------
# Top-level targets
# ---------------------------------------------------------------------------
BINARIES = \
    $(OUTPUT_DIR)/sensor_process   \
    $(OUTPUT_DIR)/compute_process  \
    $(OUTPUT_DIR)/safety_process   \
    $(OUTPUT_DIR)/logger_process   \
    $(OUTPUT_DIR)/watchdog_process

.PHONY: all clean rebuild list

all: $(BINARIES)
	@echo ""
	@echo "=== AGM build complete [$(CONFIG_NAME)] ==="
	@ls -lh $(BINARIES)

clean:
	rm -rf $(OUTPUT_DIR)
	@echo "Cleaned $(OUTPUT_DIR)"

rebuild: clean all

list:
	@echo "Binaries produced:"
	@for b in $(BINARIES); do echo "  $$b"; done

# ---------------------------------------------------------------------------
# Auto-generated dependency files (.d) — included after all rules
# so they never accidentally shadow a rule defined above.
# ---------------------------------------------------------------------------
ALL_OBJS = $(SENSOR_OBJS) $(COMPUTE_OBJS) $(SAFETY_OBJS) \
           $(LOGGER_OBJS) $(WATCHDOG_OBJS)

-include $(ALL_OBJS:.o=.d)
