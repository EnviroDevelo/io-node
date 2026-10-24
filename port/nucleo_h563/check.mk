# check.mk - compile and link check of the NUCLEO-H563ZI bench board build (port/nucleo_h563)
# with arm-none-eabi-gcc against ST's H5 HAL and CMSIS headers. Not the bench board's real build:
# that is the STM32CubeIDE project (port/nucleo_h563/CUBEMX_SETUP.md).
#   make bench EXT=/path/to/ext   (default ../ext next to this repository)
# EXT holds: xpack-arm-none-eabi-gcc-13.3.1-1.1, stm32h5xx_hal_driver, cmsis_device_h5, cmsis-core
# (the GitHub repositories STMicroelectronics/stm32h5xx_hal_driver, cmsis_device_h5, cmsis-core).
PLAT   ?= ../cems-platform
EXT    ?= ../ext
ARMCC  ?= $(EXT)/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin/arm-none-eabi-gcc
HAL     = $(EXT)/stm32h5xx_hal_driver
DEV     = -DSTM32H563xx -DUSE_HAL_DRIVER -DNODE_RS485_USART=USART2 -DNODE_RS485_IRQHandler=USART2_IRQHandler
CFLAGS  = -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -std=c11 -O2 -g -Wall -Wextra -Wshadow \
          -Wconversion -Wno-sign-conversion -Werror -ffunction-sections -fdata-sections $(EXTRA)
INC     = -Iport/nucleo_h563/check -Iport/nucleo_h563 -Iport/stm32g0 -Inode -I$(PLAT)/platform/include \
          -I$(HAL)/Inc -I$(EXT)/cmsis_device_h5/Include -I$(EXT)/cmsis-core/CMSIS/Core/Include
SRCS    = node/node.c $(PLAT)/platform/core/mb_rtu.c port/stm32g0/rs485_g0.c \
          port/nucleo_h563/bench_main.c port/nucleo_h563/bench_hw.c port/nucleo_h563/board_nucleo.c \
          port/nucleo_h563/vcp_h5.c port/nucleo_h563/check/check_main.c
HALSRC  = $(EXT)/cmsis_device_h5/Source/Templates/system_stm32h5xx.c \
          $(addprefix $(HAL)/Src/stm32h5xx_hal, .c _cortex.c _rcc.c _rcc_ex.c _gpio.c _pwr.c _pwr_ex.c)
START   = $(EXT)/cmsis_device_h5/Source/Templates/gcc/startup_stm32h563xx.s
LD      = $(EXT)/cmsis_device_h5/Source/Templates/gcc/linker/STM32H563xx_FLASH.ld
OUT    ?= build/bench
OBJS    = $(addprefix $(OUT)/,$(notdir $(SRCS:.c=.o)))
HOBJS   = $(addprefix $(OUT)/hal/,$(notdir $(HALSRC:.c=.o)))
vpath %.c $(sort $(dir $(SRCS)))

$(OUT)/bench.elf: $(OBJS) $(HOBJS) $(OUT)/startup.o
	$(ARMCC) -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard --specs=nano.specs --specs=nosys.specs \
	    -Wl,--gc-sections -T$(LD) -Wl,-Map=$(OUT)/bench.map $(OBJS) $(HOBJS) $(OUT)/startup.o -o $@
	@echo "bench board compile and link check OK: $(words $(OBJS)) node files"
	@$(dir $(ARMCC))arm-none-eabi-size $@

$(OUT)/%.o: %.c | $(OUT)
	$(ARMCC) $(CFLAGS) $(DEV) $(INC) -c $< -o $@
$(OUT)/hal/%.o: $(HAL)/Src/%.c | $(OUT)/hal
	$(ARMCC) $(filter-out -Werror -Wconversion -Wshadow -Wextra,$(CFLAGS)) $(DEV) $(INC) -c $< -o $@
$(OUT)/hal/system_stm32h5xx.o: $(EXT)/cmsis_device_h5/Source/Templates/system_stm32h5xx.c | $(OUT)/hal
	$(ARMCC) $(filter-out -Werror -Wconversion -Wshadow -Wextra,$(CFLAGS)) $(DEV) $(INC) -c $< -o $@
$(OUT)/startup.o: $(START) | $(OUT)
	$(ARMCC) -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -c $< -o $@
$(OUT) $(OUT)/hal:
	mkdir -p $@
