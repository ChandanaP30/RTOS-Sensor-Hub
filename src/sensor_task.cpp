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
//
// Fault injection behaviour (important):
//   SENSOR_STOP — the task continues looping at its normal rate but skips
//     both the heartbeat update and the queue send.  This simulates a task
//     that is alive (not crashed, not in an infinite loop) but has stopped
//     doing useful work.  The supervisor detects the missing heartbeat.
//     Data flow into Queue A also stops; the processing task will see no
//     new readings during this period.
//
//   SENSOR_SLOW — a vTaskDelay(SLOW_SENSOR_EXTRA_DELAY_MS) is inserted
//     before the heartbeat update and queue send.  After this extra delay,
//     lastWakeTime is reset to the current tick so that the subsequent
//     vTaskDelayUntil() correctly waits a full SENSOR_PERIOD_MS before
//     the next cycle.  Total effective period = SLOW_SENSOR_EXTRA_DELAY_MS
//     + SENSOR_PERIOD_MS (nominally 3000 + 1000 = 4000 ms).  Without
//     resetting lastWakeTime the vTaskDelayUntil deadline would already be
//     in the past and the task would spin immediately, making the period
//     unpredictable.
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
        //
        // Insert a blocking delay BEFORE the heartbeat and queue send so the
        // supervisor can detect that the task is running but slower than its
        // configured deadline.  vTaskDelay() yields the CPU — other tasks
        // run normally during this wait.
        //
        // After the extra delay we MUST reset lastWakeTime to the current
        // tick.  Without this reset, the subsequent vTaskDelayUntil() would
        // find that its deadline (lastWakeTime + SENSOR_PERIOD_MS) is already
        // in the past and return immediately, causing the task to spin several
        // times before its timing grid re-synchronises.  Resetting ensures
        // the next period is a predictable SENSOR_PERIOD_MS from now.
        //
        // Effective total period in slow mode:
        //   SLOW_SENSOR_EXTRA_DELAY_MS + SENSOR_PERIOD_MS = ~4000 ms
        // ------------------------------------------------------------------
        if (g_fault_sensor_slow) {
            vTaskDelay(MS_TO_TICKS(SLOW_SENSOR_EXTRA_DELAY_MS));
            // Re-anchor the periodic grid so vTaskDelayUntil is not stale
            lastWakeTime = xTaskGetTickCount();
        }

        // ------------------------------------------------------------------
        // Fault injection: SENSOR_STOP
        //
        // Skip BOTH the heartbeat update AND the queue send this cycle.
        // The task is NOT killed — it continues looping at its normal rate.
        // This simulates a task that is alive but has stopped doing useful
        // work (e.g., stuck waiting on a peripheral that never responds).
        //
        // Effect on the system:
        //   • g_hb_sensor.last_beat_ms stays frozen → supervisor detects fault.
        //   • No new SensorData is pushed to Queue A → processing task sees
        //     no readings while SENSOR_STOP is active.
        //
        // These two effects are intentional and consistent: a "stopped" task
        // neither reports its health nor produces output.
        // ------------------------------------------------------------------
        if (g_fault_sensor_stop) {
            // Skip heartbeat and data send; just wait for the next period
            vTaskDelayUntil(&lastWakeTime, MS_TO_TICKS(SENSOR_PERIOD_MS));
            continue;
        }

        // ------------------------------------------------------------------
        // Normal operation: update heartbeat
        // ------------------------------------------------------------------
        g_hb_sensor.last_beat_ms = millis();
        g_hb_sensor.beat_count++;

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
