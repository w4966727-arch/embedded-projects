######################################
# ??
######################################
TARGET = 01-test

######################################
# ??
######################################
# ???????????? ~/embedded-projects ???? make COMMON_DIR=/path ??
COMMON_DIR ?= $(HOME)/embedded-projects/common

BUILD_DIR = build
LDSCRIPT = $(COMMON_DIR)/Linker/STM32F411CEUx_FLASH.ld

######################################
# ???
######################################
C_SOURCES = \
main.c \
include.c \
ringbuf.c \
$(COMMON_DIR)/FreeRTOS/tasks.c \
$(COMMON_DIR)/FreeRTOS/queue.c \
$(COMMON_DIR)/FreeRTOS/list.c \
$(COMMON_DIR)/FreeRTOS/timers.c \
$(COMMON_DIR)/FreeRTOS/event_groups.c \
$(COMMON_DIR)/FreeRTOS/stream_buffer.c \
$(COMMON_DIR)/FreeRTOS/portable/GCC/ARM_CM4F/port.c \
$(COMMON_DIR)/FreeRTOS/portable/MemMang/heap_4.c \
$(COMMON_DIR)/Core/system_stm32f4xx.c \
$(COMMON_DIR)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c \
$(COMMON_DIR)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c \
$(COMMON_DIR)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c \
$(COMMON_DIR)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c \
$(COMMON_DIR)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c \
$(COMMON_DIR)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c \
$(COMMON_DIR)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c \
$(COMMON_DIR)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_iwdg.c \

ASM_SOURCES = \
$(COMMON_DIR)/Startup/startup_stm32f411xe.s

######################################
# ???
######################################
PREFIX = arm-none-eabi-
CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
CP = $(PREFIX)objcopy
SZ = $(PREFIX)size
HEX = $(CP) -O ihex
BIN = $(CP) -O binary -S

######################################
# ????
######################################
CPU = -mcpu=cortex-m4
FPU = -mfpu=fpv4-sp-d16
FLOAT-ABI = -mfloat-abi=hard
MCU = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

# ?
C_DEFS = -DUSE_HAL_DRIVER -DSTM32F411xE

# ?????
C_INCLUDES = \
-I. \
-I$(COMMON_DIR)/Core \
-I$(COMMON_DIR)/Drivers/STM32F4xx_HAL_Driver/Inc \
-I$(COMMON_DIR)/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy \
-I$(COMMON_DIR)/Drivers/CMSIS/Device/ST/STM32F4xx/Include \
-I$(COMMON_DIR)/Drivers/CMSIS/Include \
-I$(COMMON_DIR)/FreeRTOS/include \
-I$(COMMON_DIR)/FreeRTOS/portable/GCC/ARM_CM4F \

OPT = -Og
ASFLAGS = $(MCU) $(OPT) -Wall -fdata-sections -ffunction-sections
CFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -std=gnu99 -Wall -fdata-sections -ffunction-sections
CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)"

# ??
LDFLAGS = $(MCU) -specs=nano.specs -specs=nosys.specs -T$(LDSCRIPT) \
          -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections \
          -lc -lm -lnosys

######################################
# ????
######################################
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(HEX) $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(BIN) $< $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*.d)
