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
       bool sensorSlow;

portENTER_CRITICAL(&g_shared_mux);
sensorSlow = g_fault_sensor_slow;
portEXIT_CRITICAL(&g_shared_mux);

if (sensorSlow) {
    vTaskDelay(MS_TO_TICKS(SLOW_SENSOR_EXTRA_DELAY_MS));

    // Re-anchor the periodic grid
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
        bool sensorStop;

portENTER_CRITICAL(&g_shared_mux);
sensorStop = g_fault_sensor_stop;
portEXIT_CRITICAL(&g_shared_mux);

if (sensorStop) {
    // Skip heartbeat and data send; just wait for the next period
    vTaskDelayUntil(&lastWakeTime, MS_TO_TICKS(SENSOR_PERIOD_MS));
    continue;
}

        // ------------------------------------------------------------------
        // Normal operation: update heartbeat.
        // Both fields are written together inside one critical section so the
        // supervisor never sees a beat_count that belongs to a different
        // last_beat_ms value.
        // ------------------------------------------------------------------
        {
            uint32_t now_ms = millis();
            portENTER_CRITICAL(&g_shared_mux);
            g_hb_sensor.last_beat_ms = now_ms;
            g_hb_sensor.beat_count++;
            portEXIT_CRITICAL(&g_shared_mux);
        }

        // ------------------------------------------------------------------
        // Read the sensor
        // ------------------------------------------------------------------
        SensorData reading;
        reading.timestamp_ms = millis();

        // Read g_fault_invalid_data under a critical section into a local
        // variable so we do not hold the lock during the DHT read below.
        bool invalidData;
        portENTER_CRITICAL(&g_shared_mux);
        invalidData = g_fault_invalid_data;
        portEXIT_CRITICAL(&g_shared_mux);

        if (invalidData) {
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
        // The overflow counter increment is protected so it is not torn
        // by a preemption between its read and write steps.
        // ------------------------------------------------------------------
        BaseType_t sent = xQueueSend(g_queue_a, &reading, 0);
        if (sent != pdTRUE) {
            portENTER_CRITICAL(&g_shared_mux);
            g_queue_a_overflow_count++;
            portEXIT_CRITICAL(&g_shared_mux);
        }

        // ------------------------------------------------------------------
        // Fault injection: QUEUE_STRESS
        // Send the same reading repeatedly to rapidly fill the queue,
        // which demonstrates overflow handling.
        // Read the flag under a critical section; loop outside the section.
        // ------------------------------------------------------------------
        bool queueStress;
        portENTER_CRITICAL(&g_shared_mux);
        queueStress = g_fault_queue_stress;
        portEXIT_CRITICAL(&g_shared_mux);

        if (queueStress) {
            for (uint32_t i = 0; i < QUEUE_STRESS_BURST; i++) {
                if (xQueueSend(g_queue_a, &reading, 0) != pdTRUE) {
                    portENTER_CRITICAL(&g_shared_mux);
                    g_queue_a_overflow_count++;
                    portEXIT_CRITICAL(&g_shared_mux);
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
