// =============================================================================
// supervisor_task.cpp  —  Runtime Task Health Monitoring (the star feature)
//
// How heartbeat monitoring works:
//   Every worker task writes millis() into its HeartbeatRecord.last_beat_ms
//   each cycle.  The supervisor wakes up every SUPERVISOR_PERIOD_MS and
//   computes:
//
//       age = current_time − last_beat_ms
//
//   If age > timeout_threshold → task is declared UNHEALTHY.
//   If a task recovers, the supervisor reports it once.
//
// Key design decisions:
//   • Supervisor has the HIGHEST priority so it cannot be starved by worker
//     tasks — even a runaway task at lower priority cannot block it.
//   • Each task has its own configurable timeout; we don't use one size fits
//     all because tasks have different periods.
//   • We track "was_healthy" per task to avoid flooding the Serial with
//     repeated fault messages on every supervisor cycle.
//   • The supervisor also inspects queue overflow counters and invalid
//     reading counts, and reports changes.
//
// Logging policy:
//   All output goes through Queue B → LoggerTask, which is the sole owner
//   of Serial.  supervisorReport() uses a timeout of 0 (non-blocking).
//   If Queue B is full the message is silently dropped; this is acceptable
//   because the status summary will be retried next cycle (every 2 s) and
//   fault transitions are only reported once per state change.  Under normal
//   operation Queue B never fills up: the Logger drains it every 200 ms.
//
// Supervisor priority and scheduling:
//   The supervisor runs at priority 5 and sleeps for SUPERVISOR_PERIOD_MS
//   (2 s) between cycles via vTaskDelayUntil().  While sleeping it is in the
//   BLOCKED state and uses zero CPU — it does NOT starve lower-priority tasks.
//   Its active work per cycle (a handful of comparisons and non-blocking
//   queue sends) completes in well under 1 ms, so it has negligible impact
//   on other tasks.
//
// FreeRTOS concepts used:
//   • vTaskDelayUntil() — periodic execution at exact intervals
//   • xQueueSend()      — push reports to Logger via Queue B (timeout = 0)
// =============================================================================

#include <Arduino.h>
#include <stdio.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Helper: push a text message to Queue B so the LoggerTask prints it.
//
// Timeout = 0 (non-blocking): if Queue B is full, the message is silently
// dropped.  We deliberately do NOT fall back to Serial.print() here because:
//   1. LoggerTask is the sole authorised owner of Serial writes.
//   2. Direct Serial calls from supervisor would interleave with logger output
//      and produce garbled lines on the UART.
//   3. The supervisor's status summary repeats every 2 s, so a single dropped
//      message is not a critical loss.  Fault-transition messages (FAULT /
//      RECOVERY) are only generated once per state change and would be retried
//      only on the next transition — an acceptable trade-off for correctness.
//
// Under normal operation Queue B never fills: the Logger drains it every
// 200 ms and the system produces at most ~5 messages per 2-second cycle.
// ---------------------------------------------------------------------------
static void supervisorReport(const char* text) {
    LogMessage msg;
    msg.timestamp_ms = millis();
    snprintf(msg.text, LOG_MSG_LEN, "%s", text);
    // timeout = 0: non-blocking.  Drop silently if queue is full.
    xQueueSend(g_queue_b, &msg, 0);
}

// ---------------------------------------------------------------------------
// Helper: check one task's heartbeat and report any state change.
//
// Reads last_beat_ms and beat_count under a critical section, then works
// entirely with local copies.  The critical section is exited before any
// queue send or snprintf call — no blocking inside the lock.
//
// Parameters:
//   hb         — pointer to the task's HeartbeatRecord
//   timeout_ms — how many ms of silence before declaring unhealthy
//   was_healthy — IN/OUT: previous health state (true = was healthy)
// Returns true if currently healthy.
// ---------------------------------------------------------------------------
static bool checkHeartbeat(
        const HeartbeatRecord* hb,
        uint32_t timeout_ms,
        bool& was_healthy) {

    // Snapshot the heartbeat record under a critical section
    uint32_t snap_beat_ms;
    uint32_t snap_beat_count;
    const char* snap_name;

    portENTER_CRITICAL(&g_shared_mux);
    snap_beat_ms    = hb->last_beat_ms;
    snap_beat_count = hb->beat_count;
    snap_name       = hb->task_name;    // pointer to string literal — safe
    portEXIT_CRITICAL(&g_shared_mux);

    // All work below uses only local variables — no lock held
    uint32_t now = millis();
    uint32_t age = now - snap_beat_ms;
    bool healthy = (age <= timeout_ms);

    char buf[LOG_MSG_LEN];

    if (!healthy && was_healthy) {
        // Transition: healthy → unhealthy
        snprintf(buf, LOG_MSG_LEN,
            "[FAULT] *** %s UNRESPONSIVE! Last beat %lums ago (timeout=%lums) ***",
            snap_name,
            (unsigned long)age,
            (unsigned long)timeout_ms);
        supervisorReport(buf);
        was_healthy = false;
    } else if (healthy && !was_healthy) {
        // Transition: unhealthy → healthy (recovery)
        snprintf(buf, LOG_MSG_LEN,
            "[RECOVERY] %s is healthy again. Beat count=%lu",
            snap_name,
            (unsigned long)snap_beat_count);
        supervisorReport(buf);
        was_healthy = true;
    }
    // If state has not changed, stay silent (no log spam)
    return healthy;
}

// ---------------------------------------------------------------------------
// supervisorTask()
// ---------------------------------------------------------------------------
void supervisorTask(void* pvParameters) {
    (void)pvParameters;

    // ------------------------------------------------------------------
    // Track the previous health state of each task (true = healthy).
    // Start as healthy so we don't fire false alarms at boot while tasks
    // are still starting up.
    // ------------------------------------------------------------------
    bool sensor_healthy     = true;
    bool processing_healthy = true;
    bool logger_healthy     = true;

    // Track previous counter values so we only report changes
    uint32_t prev_qa_overflow = 0;
    uint32_t prev_qb_overflow = 0;
    uint32_t prev_invalid     = 0;

    // Let all tasks start and produce their first heartbeats
    vTaskDelay(MS_TO_TICKS(SUPERVISOR_PERIOD_MS));

    TickType_t lastWakeTime = xTaskGetTickCount();

    while (true) {

        uint32_t now = millis();
        char buf[LOG_MSG_LEN];

        // ------------------------------------------------------------------
        // Check each worker task's heartbeat
        // ------------------------------------------------------------------
        checkHeartbeat(&g_hb_sensor,     SENSOR_HEARTBEAT_TIMEOUT_MS,     sensor_healthy);
        checkHeartbeat(&g_hb_processing, PROCESSING_HEARTBEAT_TIMEOUT_MS, processing_healthy);
        checkHeartbeat(&g_hb_logger,     LOGGER_HEARTBEAT_TIMEOUT_MS,     logger_healthy);

        // ------------------------------------------------------------------
        // Check for new queue overflow events and invalid reading count.
        // Snapshot all three counters in one critical section so the values
        // are consistent with each other.
        // ------------------------------------------------------------------
        uint32_t qa, qb, inv;
        portENTER_CRITICAL(&g_shared_mux);
        qa  = g_queue_a_overflow_count;
        qb  = g_queue_b_overflow_count;
        inv = g_invalid_reading_count;
        portEXIT_CRITICAL(&g_shared_mux);

        if (qa != prev_qa_overflow) {
            snprintf(buf, LOG_MSG_LEN,
                "[FAULT] Queue A overflow! Total lost packets: %lu",
                (unsigned long)qa);
            supervisorReport(buf);
            prev_qa_overflow = qa;
        }
        if (qb != prev_qb_overflow) {
            snprintf(buf, LOG_MSG_LEN,
                "[FAULT] Queue B overflow! Total lost log messages: %lu",
                (unsigned long)qb);
            supervisorReport(buf);
            prev_qb_overflow = qb;
        }

        // ------------------------------------------------------------------
        // Check for new invalid readings (inv was already snapshot above)
        // ------------------------------------------------------------------
        if (inv != prev_invalid) {
            snprintf(buf, LOG_MSG_LEN,
                "[WARN] Invalid sensor readings detected. Total: %lu",
                (unsigned long)inv);
            supervisorReport(buf);
            prev_invalid = inv;
        }

        // ------------------------------------------------------------------
        // Periodic status summary (every SUPERVISOR_PERIOD_MS)
        // This confirms the supervisor itself is alive and working.
        // ------------------------------------------------------------------
        snprintf(buf, LOG_MSG_LEN,
            "[SUPER] t=%lums | Sensor:%s Processing:%s Logger:%s | QA_OVF:%lu QB_OVF:%lu INV:%lu",
            (unsigned long)now,
            sensor_healthy     ? "OK" : "FAIL",
            processing_healthy ? "OK" : "FAIL",
            logger_healthy     ? "OK" : "FAIL",
            (unsigned long)qa,
            (unsigned long)qb,
            (unsigned long)inv);
        supervisorReport(buf);

        // ------------------------------------------------------------------
        // Sleep until next supervisor cycle
        // ------------------------------------------------------------------
        vTaskDelayUntil(&lastWakeTime, MS_TO_TICKS(SUPERVISOR_PERIOD_MS));
    }
    vTaskDelete(nullptr);
}
