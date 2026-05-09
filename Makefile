CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -MMD -MP \
	-Isrc -Itests -I../third_party/doctest
LDFLAGS := -lcurl

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
TEST_TARGET := $(BUILD_DIR)/pqueue-tests

PQUEUE_SRC := \
	src/pqueue/envelope.cpp \
	src/pqueue/http/esp32_arduino_transport.cpp \
	src/pqueue/http/outbox.cpp \
	src/pqueue/http/posix_curl_transport.cpp \
	src/pqueue/http/request_envelope.cpp \
	src/pqueue/file_store.cpp \
	src/pqueue/diagnostics.cpp \
	src/pqueue/storage_common.cpp \
	src/pqueue/storage_posix.cpp \
	src/pqueue/storage_littlefs.cpp \
	src/pqueue/outbox.cpp \
	src/pqueue/queue.cpp

TEST_SRC := \
	tests/posix/main.cpp \
	tests/posix/pqueue.cpp \
	tests/posix/pqueue_diagnostics.cpp \
	tests/posix/pqueue_envelope.cpp \
	tests/posix/pqueue_file_store.cpp \
	tests/posix/pqueue_http_outbox.cpp \
	tests/posix/pqueue_http_request_envelope.cpp \
	tests/posix/pqueue_outbox.cpp \
	tests/posix/pqueue_repair.cpp \
	tests/posix/pqueue_queue_edges.cpp \
	$(PQUEUE_SRC)

OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(TEST_SRC))

.PHONY: all test tests run-tests clean

all: test

test: run-tests

tests: run-tests

run-tests: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJ:.o=.d)
