# Host test harness: builds each test_*.c into its own binary and runs it.
# Usage: make -C host_tests test

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
HERE      := $(ROOT)/host_tests
BUILD     := $(HERE)/build
UNITY_DIR := $(HERE)/third_party/unity
CJSON_DIR := $(HERE)/third_party/cjson

CC     ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -g -O0
# Third-party code is compiled without -Werror and without -Wextra noise.
TP_CFLAGS := -std=c11 -D_DEFAULT_SOURCE -g -O0 -w

INCLUDES := -I$(ROOT)/components/hy_core/include -I$(UNITY_DIR) -I$(CJSON_DIR)
ifneq ($(wildcard $(ROOT)/components/hy_store/include),)
INCLUDES += -I$(ROOT)/components/hy_store/include
endif

CORE_SRCS  := $(wildcard $(ROOT)/components/hy_core/src/*.c)
STORE_SRCS := $(wildcard $(ROOT)/components/hy_store/src/store.c)
PROJ_SRCS  := $(CORE_SRCS) $(STORE_SRCS)
PROJ_OBJS  := $(patsubst $(ROOT)/%.c,$(BUILD)/obj/%.o,$(PROJ_SRCS))
TP_OBJS    := $(BUILD)/obj/unity.o $(BUILD)/obj/cJSON.o

TEST_SRCS := $(sort $(wildcard $(HERE)/test_*.c))
TEST_BINS := $(patsubst $(HERE)/%.c,$(BUILD)/%,$(TEST_SRCS))

.PHONY: test clean
test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
	  echo "== $$t"; $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; echo "ALL TESTS PASSED"

$(BUILD)/%: $(HERE)/%.c $(PROJ_OBJS) $(TP_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(PROJ_OBJS) $(TP_OBJS) -lm

$(BUILD)/obj/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/obj/unity.o: $(UNITY_DIR)/unity.c
	@mkdir -p $(dir $@)
	$(CC) $(TP_CFLAGS) -I$(UNITY_DIR) -c -o $@ $<

$(BUILD)/obj/cJSON.o: $(CJSON_DIR)/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(TP_CFLAGS) -I$(CJSON_DIR) -c -o $@ $<

clean:
	rm -rf $(BUILD)
