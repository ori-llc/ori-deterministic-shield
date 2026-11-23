PREFIX ?= arm-none-eabi-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size

BUILD_DIR := build
TARGET    := $(BUILD_DIR)/ori_shield_f401re

CFLAGS := -mcpu=cortex-m4 -mthumb -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
          -ffreestanding -fno-builtin -fdata-sections -ffunction-sections \
          -I../../include
LDFLAGS := -T stm32f401re.ld -nostartfiles -nostdlib -Wl,--gc-sections -lgcc

SRCS := startup_stm32f401re.c main.c ../../src/ori_shield.c

.PHONY: all clean flash

all: $(TARGET).bin

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET).elf: $(SRCS) stm32f401re.ld ../../include/ori_shield.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

flash: $(TARGET).bin
	st-flash --reset write $(TARGET).bin 0x08000000

clean:
	rm -rf $(BUILD_DIR)
