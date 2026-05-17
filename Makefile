CXX := g++
CLI11_INCLUDE ?= third_party
DOCTEST_INCLUDE ?= ../third_party/doctest

CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -MMD -MP \
	-Isrc -Itests -I$(DOCTEST_INCLUDE) -I$(CLI11_INCLUDE)
LDFLAGS := -lcurl

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
TEST_TARGET := $(BUILD_DIR)/pqueue-tests
REPAIR_TOOL_TARGET := $(BUILD_DIR)/pqueue-repair-tool
PROFILING_TARGET := $(BUILD_DIR)/pqueue-profiling
SIM_TARGET := $(BUILD_DIR)/pqueue-compaction-sim

PQUEUE_SRC := \
	src/pqueue/append_log_common.cpp \
	src/pqueue/append_log_store.cpp \
	src/pqueue/envelope.cpp \
	src/pqueue/http/esp32_arduino_transport.cpp \
	src/pqueue/http/outbox.cpp \
	src/pqueue/http/posix_curl_transport.cpp \
	src/pqueue/http/request_envelope.cpp \
	src/pqueue/file_store.cpp \
	src/pqueue/internal/lock_owner.cpp \
	src/pqueue/diagnostics.cpp \
	src/pqueue/storage_common.cpp \
	src/pqueue/storage_posix.cpp \
	src/pqueue/storage_littlefs.cpp \
	src/pqueue/outbox.cpp \
	src/pqueue/queue.cpp

TEST_SRC := \
	tests/posix/main.cpp \
	tests/posix/pqueue.cpp \
	tests/posix/pqueue_append_log.cpp \
	tests/posix/pqueue_diagnostics.cpp \
	tests/posix/pqueue_envelope.cpp \
	tests/posix/pqueue_file_store.cpp \
	tests/posix/pqueue_full_queue_policy.cpp \
	tests/posix/pqueue_http_outbox.cpp \
	tests/posix/pqueue_http_request_envelope.cpp \
	tests/posix/pqueue_outbox.cpp \
	tests/posix/pqueue_rebuild_metadata.cpp \
	tests/posix/pqueue_repair.cpp \
	tests/posix/pqueue_queue_edges.cpp \
	$(PQUEUE_SRC)

OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(TEST_SRC))

DOCS_DIR := docs
DOCS_MD := $(wildcard $(DOCS_DIR)/*.md)
DOCS_PDF := $(DOCS_MD:.md=.pdf)

.PHONY: all test tests run-tests repair-tool profiling sim docs clean .FORCE

all: test

docs: $(DOCS_PDF)

$(DOCS_DIR)/%.pdf: $(DOCS_DIR)/%.md .FORCE
	pandoc $< -o $@ --pdf-engine=xelatex -V monofont="DejaVu Sans Mono"

test: run-tests

tests: run-tests

run-tests: $(TEST_TARGET)
	./$(TEST_TARGET)

repair-tool: $(REPAIR_TOOL_TARGET)

profiling: $(PROFILING_TARGET)

$(PROFILING_TARGET): tools/pqueue_profiling.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

sim: $(SIM_TARGET)

$(SIM_TARGET): tools/pqueue_compaction_sim.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Itools $^ -o $@ $(LDFLAGS)

$(REPAIR_TOOL_TARGET): tools/pqueue_repair_tool.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJ:.o=.d)
