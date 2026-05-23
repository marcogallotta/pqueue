CXX := g++
CLI11_INCLUDE ?= third_party
DOCTEST_INCLUDE ?= ../third_party/doctest

CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -O2 -MMD -MP \
	-Isrc -Itests -I$(DOCTEST_INCLUDE) -I$(CLI11_INCLUDE)
LDFLAGS := -lcurl

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
TEST_TARGET := $(BUILD_DIR)/pqueue-tests
COV_DIR := $(BUILD_DIR)/coverage
COV_TARGET := $(COV_DIR)/pqueue-tests-cov
EXAMPLE_BASIC_QUEUE := $(BUILD_DIR)/examples/basic-queue
EXAMPLE_OUTBOX := $(BUILD_DIR)/examples/outbox
REPAIR_TOOL_TARGET := $(BUILD_DIR)/pqueue-repair-tool
APPENDLOG_DIAG_TARGET := $(BUILD_DIR)/pqueue-appendlog-diag
DOCTOR_DUMP_TARGET := $(BUILD_DIR)/pqueue-doctor-dump
PROFILING_TARGET  := $(BUILD_DIR)/pqueue-profiling
SIM_TARGET        := $(BUILD_DIR)/pqueue-compaction-sim
BENCHMARK_TARGET  := $(BUILD_DIR)/pqueue-benchmark

PQUEUE_SRC := \
	src/pqueue/append_log_common.cpp \
	src/pqueue/append_log_store/store.cpp \
	src/pqueue/append_log_store/manifest.cpp \
	src/pqueue/append_log_store/compaction.cpp \
	src/pqueue/envelope.cpp \
	src/pqueue/http/esp32_arduino_transport.cpp \
	src/pqueue/http/outbox.cpp \
	src/pqueue/http/posix_curl_transport.cpp \
	src/pqueue/http/request_envelope.cpp \
	src/pqueue/internal/lock_owner.cpp \
	src/pqueue/diagnostics.cpp \
	src/pqueue/storage_posix.cpp \
	src/pqueue/storage_littlefs.cpp \
	src/pqueue/outbox.cpp \
	src/pqueue/queue.cpp

TEST_SRC := \
	tests/posix/main.cpp \
	tests/posix/pqueue.cpp \
	tests/posix/pqueue_append_log.cpp \
	tests/posix/pqueue_append_log_manifest.cpp \
	tests/posix/pqueue_append_log_rollover.cpp \
	tests/posix/pqueue_append_log_compaction.cpp \
	tests/posix/pqueue_append_log_seq_edges.cpp \
	tests/posix/pqueue_append_log_validate.cpp \
	tests/posix/pqueue_envelope.cpp \
	tests/posix/pqueue_full_queue_policy.cpp \
	tests/posix/pqueue_http_outbox.cpp \
	tests/posix/pqueue_http_request_envelope.cpp \
	tests/posix/pqueue_outbox.cpp \
	tests/posix/pqueue_repair.cpp \
	tests/posix/pqueue_queue_edges.cpp \
	$(PQUEUE_SRC)

OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(TEST_SRC))

DOCS_DIR := docs
DOCS_MD := $(wildcard $(DOCS_DIR)/*.md)
DOCS_PDF := $(DOCS_MD:.md=.pdf)

.PHONY: all test tests run-tests examples run-examples repair-tool appendlog-diag doctor-dump profiling sim benchmark docs coverage clean .FORCE

all: test

docs: $(DOCS_PDF)

$(DOCS_DIR)/%.pdf: $(DOCS_DIR)/%.md .FORCE
	pandoc $< -o $@ --pdf-engine=xelatex -V monofont="DejaVu Sans Mono"

test: run-tests

tests: run-tests

run-tests: $(TEST_TARGET)
	./$(TEST_TARGET)

examples: $(EXAMPLE_BASIC_QUEUE) $(EXAMPLE_OUTBOX)

run-examples: examples
	./$(EXAMPLE_BASIC_QUEUE)
	./$(EXAMPLE_OUTBOX)

$(EXAMPLE_BASIC_QUEUE): examples/basic_queue.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(EXAMPLE_OUTBOX): examples/outbox.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

repair-tool: $(REPAIR_TOOL_TARGET)

appendlog-diag: $(APPENDLOG_DIAG_TARGET)

doctor-dump: $(DOCTOR_DUMP_TARGET)

profiling: $(PROFILING_TARGET)

$(PROFILING_TARGET): tools/pqueue_profiling.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Itools $^ -o $@ $(LDFLAGS)

benchmark: $(BENCHMARK_TARGET)

$(BENCHMARK_TARGET): tools/pqueue_benchmark.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Itools $^ -o $@ $(LDFLAGS)

sim: $(SIM_TARGET)

$(SIM_TARGET): tools/pqueue_compaction_sim.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Itools $^ -o $@ $(LDFLAGS)

$(REPAIR_TOOL_TARGET): tools/pqueue_repair_tool.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(APPENDLOG_DIAG_TARGET): tools/pqueue_appendlog_diag.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(DOCTOR_DUMP_TARGET): tools/pqueue_doctor_dump_posix.cpp $(PQUEUE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Itools $^ -o $@ $(LDFLAGS)

$(TEST_TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

COV_OBJ := $(patsubst %.cpp,$(COV_DIR)/obj/%.o,$(TEST_SRC))

coverage: $(COV_TARGET)
	./$(COV_TARGET) || true
	gcovr --root . --object-directory $(COV_DIR)/obj \
	      --filter src/ --html-details $(COV_DIR)/html/index.html
	@echo "Report: $(COV_DIR)/html/index.html"

$(COV_TARGET): $(COV_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $^ -o $@ $(LDFLAGS) --coverage

$(COV_DIR)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -O0 --coverage -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJ:.o=.d)
