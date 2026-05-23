// ESP32 HTTP outbox -- reference sketch for pqueue::http::Outbox on Arduino/LittleFS.
//
// This file is a reference for firmware integration. It does not build on
// desktop. For a runnable example, see examples/outbox.cpp.
//
// Key differences from the POSIX examples:
//   - storageBackend = LittleFS (not default POSIX)
//   - esp_timer_get_time()/1000 for the monotonic clock
//   - esp_task_wdt_reset() / vTaskDelay() to yield the watchdog during compaction

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>

#include "pqueue/http/outbox.h"
#include "pqueue/http/esp32_arduino_transport.h"

// --- Configuration ---

static pqueue::http::Config makeConfig() {
    pqueue::http::Config cfg;

    // Queue storage
    cfg.queue.storageBackend  = pqueue::StorageBackend::LittleFS;
    cfg.queue.basePath        = "/outbox";
    cfg.queue.reservedBytes   = 65536;    // 64 KB logical cap for queue segment files
    cfg.queue.maxSegmentBytes = 4096;     // 4 KB per segment (matches LittleFS block boundary)
    cfg.queue.minFreeBytes    = 8192;     // leave 8 KB headroom for LittleFS metadata
    cfg.queue.maxSegments     = 16;

    // Outbox retry policy
    cfg.outbox.retryDelayMs              = 15000;  // 15 s between retry attempts
    cfg.outbox.maxDrainAttemptsPerSecond = 2;

    cfg.baseUrl = "https://api.example.com";

    return cfg;
}

static std::uint64_t monotonicMs(void*) {
    return esp_timer_get_time() / 1000;
}

// --- TLS transport ---
// Pass a CA cert file so TLS validation works without disabling certificate checks.

static pqueue::http::Esp32ArduinoTransportConfig transportConfig() {
    pqueue::http::Esp32ArduinoTransportConfig cfg;
    cfg.caCertPath       = "/certs/server.pem";
    cfg.caCertFileSystem = &LittleFS;
    return cfg;
}

static pqueue::http::Esp32ArduinoTransport transport(transportConfig());
static pqueue::http::Outbox outbox(makeConfig(), transport, monotonicMs, nullptr);

// Track whether the previous drain left more compaction work pending.
static bool moreCompactionWork = false;

// Interval between sensor submissions. submitPost() is not called every loop()
// tick — that would flood the queue with thousands of readings per second.
static const unsigned long kSubmitIntervalMs = 30000;  // 30 s
static unsigned long lastSubmitMs = 0;

// --- Arduino lifecycle ---

void setup() {
    LittleFS.begin(true);
}

void loop() {
    // Enqueue a sensor reading on a fixed interval, not every tick.
    const unsigned long now = millis();
    if (now - lastSubmitMs >= kSubmitIntervalMs) {
        lastSubmitMs = now;
        outbox.submitPost("/readings", R"({"sensor":"temp","v":22.1})");
    }

    // Drain: attempt up to 5 deliveries per loop tick. The rate limiter
    // (maxDrainAttemptsPerSecond) prevents flooding the backend.
    const auto dr = outbox.drainUpTo(5);

    // Compact after a drain that freed records, or continue a previous pass.
    // Each compactIdle(1) call is one bounded step; yield the watchdog between
    // steps so the loop stays responsive.
    if (dr.removedQueuedBytes > 0 || moreCompactionWork) {
        pqueue::CompactIdleResult cr;
        do {
            cr = outbox.compactIdle(1);
            esp_task_wdt_reset();
            vTaskDelay(1);
        } while (cr.compactions > 0);
        moreCompactionWork = cr.moreWorkLikely;
    }
}
