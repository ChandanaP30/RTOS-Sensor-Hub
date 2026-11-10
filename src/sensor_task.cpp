// =============================================================================
// sensor_task.cpp  —  Reads the DHT22 sensor and feeds Queue A
//
// Responsibilities:
//   • Read temperature and humidity from DHT22 every SENSOR_PERIOD_MS.
//   • Validate readings (NaN check).
//   • Send a SensorData struct to Queue A for the Processing Task.
//   • Update its own heartbeat (unless SENSOR_STOP fault is active).
//   • Count failed queue sends (overflow).
//
// FreeRTOS concepts used:
//   • vTaskDelayUntil()  — strict periodic execution (not just "sleep 1 s")
//   • xQueueSend()       — non-blocking queue write (timeout = 0)
//   • Heartbeat pattern  — simple timestamp updated each cycle
// =============================================================================

#include <Arduino.h>
#include <DHT.h>
#include "config.h"

// ---------------------------------------------------------------------------
// DHT22 sensor object (uses the Adafruit DHT library)
// DHT_PIN and DHT_TYPE are defined in config.h
// ---------------------------------------------------------------------------
static DHT dht(DHT_PIN, DHT_TYPE);

// ---------------------------------------------------------------------------
// sensorTask()
//
// pvParameters: unused (always nullptr from main.cpp)
// ---------------------------------------------------------------------------
void sensorTask(void* pvParameters) {
    (void)pvParameters;   // Suppress "unused parameter" compiler warning

    // Initialise the DHT sensor driver
    dht.begin();

    // vTaskDelayUntil() needs to know when the task last woke up.
    // We initialise it to "now" so the first period starts immediately.
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (true) {

        // ------------------------------------------------------------------
        // Fault injection: SENSOR_SLOW
        // Add an extra delay BEFORE the heartbeat update so the supervisor
        // can detect that the task is running but slower than expected.
        // We use vTaskDelay() (not a busy-wait) so other tasks still run.
        // ------------------------------------------------------------------
        if (g_fault_sensor_slow) {
            vTaskDelay(MS_TO_TICKS(SLOW_SENSOR_EXTRA_DELAY_MS));
        }

        // ------------------------------------------------------------------
        // Fault injection: SENSOR_STOP
        // Simply skip the heartbeat update this cycle.
        // The task is NOT killed — it keeps looping at normal speed.
        // The supervisor will notice that last_beat_ms is not updating.
        // ------------------------------------------------------------------
        if (!g_fault_sensor_stop) {
            // Normal operation — beat the heartbeat
            g_hb_sensor.last_beat_ms = millis();
            g_hb_sensor.beat_count++;
        }
        // If SENSOR_STOP: heartbeat stays frozen while the task keeps running.

        // ------------------------------------------------------------------
        // Read the sensor
        // ------------------------------------------------------------------
        SensorData reading;
        reading.timestamp_ms = millis();

        if (g_fault_invalid_data) {
            // Fault injection: force NaN values so the Processing Task
            // has something invalid to detect and report.
            reading.temperature = NAN;
            reading.humidity    = NAN;
            reading.valid       = false;
        } else {
            // Normal read from the DHT22 driver
            reading.temperature = dht.readTemperature();  // Celsius
            reading.humidity    = dht.readHumidity();     // %RH

            // isnan() is a standard C math function: true when value is NaN.
            // The DHT library returns NaN when communication with the sensor
            // fails (e.g., wrong pin, timing issue).
            reading.valid = !(isnan(reading.temperature) || isnan(reading.humidity));
        }

        // ------------------------------------------------------------------
        // Send reading to Queue A
        // xQueueSend() copies the struct into the queue by value.
        // Timeout = 0: if the queue is full, we do NOT block — we just
        // count the failure and move on.  Blocking here could cause a
        // priority inversion if the Processing Task is delayed.
        // ------------------------------------------------------------------
        BaseType_t sent = xQueueSend(g_queue_a, &reading, 0);
        if (sent != pdTRUE) {
            // Queue was full — count the overflow
            g_queue_a_overflow_count++;
        }

        // ------------------------------------------------------------------
        // Fault injection: QUEUE_STRESS
        // Send the same reading repeatedly to rapidly fill the queue,
        // which demonstrates overflow handling.
        // ------------------------------------------------------------------
        if (g_fault_queue_stress) {
            for (uint32_t i = 0; i < QUEUE_STRESS_BURST; i++) {
                if (xQueueSend(g_queue_a, &reading, 0) != pdTRUE) {
                    g_queue_a_overflow_count++;
                }
            }
        }

        // ------------------------------------------------------------------
        // vTaskDelayUntil()
        //
        // This is different from vTaskDelay():
        //   vTaskDelay(X)       — sleep for X ticks FROM NOW (drift occurs).
        //   vTaskDelayUntil()   — sleep UNTIL lastWakeTime + period.
        //
        // vTaskDelayUntil() compensates for the time the task body took,
        // so the period stays accurate regardless of execution time.
        // This is the correct way to implement periodic tasks in FreeRTOS.
        // ------------------------------------------------------------------
        vTaskDelayUntil(&lastWakeTime, MS_TO_TICKS(SENSOR_PERIOD_MS));
    }
    // A FreeRTOS task should never return.  If we reach here, delete self.
    vTaskDelete(nullptr);
}
