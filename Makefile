# Host build and tests for the CEMS I/O board node firmware.
# The platform repository is expected next to this one (CEMS Supervisor\Firmware\cems-platform);
# override with  make PLAT=/path/to/cems-platform.
#
#   make test         node core tests + byte-for-byte comparison with the platform's simulated node
#   make master-test  the platform's whole host test suite, with every simulated I/O board
#                     replaced by this node firmware (tests/platform/iob_node_real.c)
#   make target       compile-check the STM32G0B1 build with arm-none-eabi-gcc (port/stm32g0/check.mk)
PLAT   ?= ../cems-platform
CC     ?= gcc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror \
          -fsanitize=address,undefined -fno-omit-frame-pointer
INC     = -Inode -Idrivers -Iport/stm32g0 -Itests -I$(PLAT)/platform/include -I$(PLAT)/platform/bsp
BUILD   = build

NODE_SRC = node/node.c $(PLAT)/platform/core/mb_rtu.c
TEST_SRC = tests/test_main.c tests/test_node.c tests/test_diff.c tests/test_drv.c tests/sim_hw.c tests/host_port.c tests/sim_chips.c $(PLAT)/platform/bsp/iob_node_sim.c
DRV_SRC  = $(wildcard drivers/*.c)
HDRS     = $(wildcard node/*.h drivers/*.h tests/*.h port/stm32g0/node_port.h) $(PLAT)/platform/include/iobus.h $(PLAT)/platform/include/mb_rtu.h

.PHONY: test master-test target vnode clean
test: $(BUILD)/node_tests
	./$(BUILD)/node_tests

$(BUILD)/node_tests: $(NODE_SRC) $(DRV_SRC) $(TEST_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DPLAT_HOST $(INC) $(NODE_SRC) $(DRV_SRC) $(TEST_SRC) -o $@ -lm

# ---- the platform's tests with the real node in place of iob_node_sim.c ----
# The platform's own Makefile builds its test binary, with its source list (CORE) read back from
# it and only iob_node_sim.c swapped out, into this repository's build directory. Paths are given
# relative to the platform folder (SELF = this repository as seen from there), because make cannot
# handle the spaces in "My Documents" or "CEMS Supervisor" in absolute paths.
SELF      ?= ../$(lastword $(notdir $(CURDIR)))
PLAT_CORE := $(shell $(MAKE) -s --no-print-directory -C $(PLAT) -f Makefile -f ../$(lastword $(notdir $(CURDIR)))/tools/printvar.mk print-CORE 2>/dev/null)
REAL_NODE  = $(SELF)/tests/platform/iob_node_real.c $(SELF)/node/node.c $(SELF)/tests/sim_hw.c
master-test:
	@test -n "$(PLAT_CORE)" || { echo "cannot read CORE from $(PLAT)/Makefile"; exit 1; }
	$(MAKE) -C $(PLAT) BUILD=$(SELF)/$(BUILD)/plat \
	    CORE="$(filter-out platform/bsp/iob_node_sim.c,$(PLAT_CORE)) $(REAL_NODE)" \
	    CFLAGS="$(CFLAGS) -I$(SELF)/node -I$(SELF)/tests" $(SELF)/$(BUILD)/plat/tests
	cd $(PLAT) && $(SELF)/$(BUILD)/plat/tests

# ---- a virtual node on a pseudo-terminal, for trying tools/at02.py without a board (Linux/macOS) ----
vnode: $(BUILD)/vnode
$(BUILD)/vnode: tests/vnode.c $(NODE_SRC) tests/sim_hw.c $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DPLAT_HOST $(INC) tests/vnode.c $(NODE_SRC) tests/sim_hw.c -o $@

target:
	$(MAKE) -f port/stm32g0/check.mk PLAT=$(PLAT)

clean:
	rm -rf $(BUILD)
