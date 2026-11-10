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
// FreeRTOS concepts used:
//   • vTaskDelayUntil() — periodic execution at exact intervals
//   • xQueueSend()      — push reports to Logger via Queue B
// =============================================================================

#include <Arduino.h>
#include <stdio.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Helper: push a text message directly to Queue B so the Logger prints it.
// If Queue B is full, we fall back to a direct Serial.print (the supervisor
// must always be able to report faults).
// ---------------------------------------------------------------------------
static void supervisorReport(const char* text) {
    LogMessage msg;
    msg.timestamp_ms = millis();
    snprintf(msg.text, LOG_MSG_LEN, "%s", text);

    if (xQueueSend(g_queue_b, &msg, MS_TO_TICKS(50)) != pdTRUE) {
        // Queue B is full — print directly as a last resort
        Serial.print(F("[SUPERVISOR-DIRECT] "));
        Serial.println(text);
    }
}

// ---------------------------------------------------------------------------
// Helper: check one task's heartbeat and report any state change.
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

    uint32_t now = millis();
    uint32_t age = now - hb->last_beat_ms;   // ms since last heartbeat
    bool healthy = (age <= timeout_ms);

    char buf[LOG_MSG_LEN];

    if (!healthy && was_healthy) {
        // Transition: healthy → unhealthy
        snprintf(buf, LOG_MSG_LEN,
            "[FAULT] *** %s UNRESPONSIVE! Last beat %lums ago (timeout=%lums) ***",
            hb->task_name,
            (unsigned long)age,
            (unsigned long)timeout_ms);
        supervisorReport(buf);
        was_healthy = false;
    } else if (healthy && !was_healthy) {
        // Transition: unhealthy → healthy (recovery)
        snprintf(buf, LOG_MSG_LEN,
            "[RECOVERY] %s is healthy again. Beat count=%lu",
            hb->task_name,
            (unsigned long)hb->beat_count);
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
        // Check for new queue overflow events
        // ------------------------------------------------------------------
        uint32_t qa = g_queue_a_overflow_count;
        uint32_t qb = g_queue_b_overflow_count;

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
        // Check for new invalid readings
        // ------------------------------------------------------------------
        uint32_t inv = g_invalid_reading_count;
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
