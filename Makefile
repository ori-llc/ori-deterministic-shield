CC ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Iinclude
BUILD_DIR := build
TEST_BIN := $(BUILD_DIR)/test_ori_shield

.PHONY: all test py-test firmware clean

all: test

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TEST_BIN): src/ori_shield.c tests/test_ori_shield.c include/ori_shield.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) src/ori_shield.c tests/test_ori_shield.c -o $(TEST_BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

py-test:
	python3 -m unittest discover -s python_mcp_guard -v

firmware:
	$(MAKE) -C firmware/nucleo_f401re

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C firmware/nucleo_f401re clean
