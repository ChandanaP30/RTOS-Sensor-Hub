// =============================================================================
// logger_task.cpp  —  Sole owner of Serial output for application logs
//
// Responsibilities:
//   • Drain Queue B and print every LogMessage to Serial.
//   • Print a timestamp prefix on each line.
//   • Update its own heartbeat.
//   • Avoid Serial contention: ONLY this task (and the supervisor's urgent
//     alerts via a separate direct Serial.print) write to UART.
//     The supervisor uses the same queue so its messages are serialised too.
//
// Design note:
//   Having exactly ONE task write to Serial prevents garbled output.
//   Other tasks build strings and push them onto Queue B; they never call
//   Serial.print() themselves.
//
// FreeRTOS concepts used:
//   • xQueueReceive() with a short timeout
//   • vTaskDelayUntil()
// =============================================================================

#include <Arduino.h>
#include <stdio.h>
#include "config.h"

void loggerTask(void* pvParameters) {
    (void)pvParameters;

    TickType_t lastWakeTime = xTaskGetTickCount();

    while (true) {

        // ------------------------------------------------------------------
        // Update heartbeat BEFORE draining the queue
        // ------------------------------------------------------------------
        g_hb_logger.last_beat_ms = millis();
        g_hb_logger.beat_count++;

        // ------------------------------------------------------------------
        // Drain all currently-available messages from Queue B.
        // We loop until the queue is empty (or we have been running for
        // too long — the vTaskDelayUntil at the bottom handles timing).
        // ------------------------------------------------------------------
        LogMessage msg;
        while (xQueueReceive(g_queue_b, &msg, 0) == pdTRUE) {

            // Print elapsed milliseconds as a timestamp prefix.
            // This is simpler than a real clock and works in Wokwi.
            char prefix[32];
            snprintf(prefix, sizeof(prefix), "[%8lums] ", (unsigned long)msg.timestamp_ms);

            Serial.print(prefix);
            Serial.println(msg.text);
        }

        // ------------------------------------------------------------------
        // Periodic delay
        // ------------------------------------------------------------------
        vTaskDelayUntil(&lastWakeTime, MS_TO_TICKS(LOGGER_PERIOD_MS));
    }
    vTaskDelete(nullptr);
}
