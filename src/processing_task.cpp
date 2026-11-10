// =============================================================================
// processing_task.cpp  —  Validates sensor data and formats log messages
//
// Responsibilities:
//   • Wait for SensorData items on Queue A.
//   • Validate the data (NaN, out-of-range checks).
//   • Build a human-readable log string.
//   • Send the LogMessage to Queue B for the Logger Task.
//   • Update its own heartbeat each cycle.
//   • Count invalid readings separately from queue failures.
//
// FreeRTOS concepts used:
//   • xQueueReceive()  — blocking receive with timeout
//   • xQueueSend()     — non-blocking send to Queue B
//   • vTaskDelayUntil() — periodic execution
// =============================================================================

#include <Arduino.h>
#include <math.h>      // isnan(), isinf()
#include <stdio.h>     // snprintf()
#include "config.h"

// ---------------------------------------------------------------------------
// Sensor data validity limits
// These are physical limits for the DHT22 sensor datasheet:
//   Temperature: -40 °C to +80 °C
//   Humidity:      0 % to 100 %
// ---------------------------------------------------------------------------
constexpr float TEMP_MIN = -40.0f;
constexpr float TEMP_MAX =  80.0f;
constexpr float HUM_MIN  =   0.0f;
constexpr float HUM_MAX  = 100.0f;

// ---------------------------------------------------------------------------
// Helper: check whether a SensorData reading passes all validity checks
// Returns true if reading is usable.
// ---------------------------------------------------------------------------
static bool isReadingValid(const SensorData& d) {
    if (!d.valid)                              return false;
    if (isnan(d.temperature) || isnan(d.humidity)) return false;
    if (isinf(d.temperature) || isinf(d.humidity)) return false;
    if (d.temperature < TEMP_MIN || d.temperature > TEMP_MAX) return false;
    if (d.humidity    < HUM_MIN  || d.humidity    > HUM_MAX)  return false;
    return true;
}

// ---------------------------------------------------------------------------
// processingTask()
// ---------------------------------------------------------------------------
void processingTask(void* pvParameters) {
    (void)pvParameters;

    TickType_t lastWakeTime = xTaskGetTickCount();

    while (true) {

        // ------------------------------------------------------------------
        // Update heartbeat BEFORE blocking on the queue so the supervisor
        // knows this task is alive even when Queue A is empty.
        // ------------------------------------------------------------------
        g_hb_processing.last_beat_ms = millis();
        g_hb_processing.beat_count++;

        // ------------------------------------------------------------------
        // Try to receive a SensorData item from Queue A.
        // We wait up to PROCESSING_PERIOD_MS before giving up this cycle.
        // This keeps the task periodic even when no data arrives.
        // ------------------------------------------------------------------
        SensorData incoming;
        BaseType_t received = xQueueReceive(
            g_queue_a,
            &incoming,
            MS_TO_TICKS(PROCESSING_PERIOD_MS)
        );

        if (received == pdTRUE) {
            // A reading arrived — validate it
            LogMessage msg;
            bool valid = isReadingValid(incoming);

            if (!valid) {
                // Count it
                g_invalid_reading_count++;

                // Build an error message
                snprintf(
                    msg.text, LOG_MSG_LEN,
                    "[DATA-ERR] Invalid reading at %lums | Temp=%.2f | Hum=%.2f",
                    (unsigned long)incoming.timestamp_ms,
                    (double)incoming.temperature,
                    (double)incoming.humidity
                );
            } else {
                // Build a normal sensor log line
                snprintf(
                    msg.text, LOG_MSG_LEN,
                    "[SENSOR]   T=%.1f°C  H=%.1f%%  | t=%lums",
                    (double)incoming.temperature,
                    (double)incoming.humidity,
                    (unsigned long)incoming.timestamp_ms
                );
            }

            msg.timestamp_ms = millis();

            // Send the formatted message to Queue B (Logger)
            if (xQueueSend(g_queue_b, &msg, 0) != pdTRUE) {
                g_queue_b_overflow_count++;
            }
        }
        // If nothing received: just move to the next period (heartbeat
        // was already updated above, so the supervisor stays happy).

        // ------------------------------------------------------------------
        // Periodic delay — aligns the next wake to the grid
        // ------------------------------------------------------------------
        vTaskDelayUntil(&lastWakeTime, MS_TO_TICKS(PROCESSING_PERIOD_MS));
    }
    vTaskDelete(nullptr);
}
