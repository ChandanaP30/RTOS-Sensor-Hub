// =============================================================================
// main.cpp  —  Entry point for the RTOS Sensor Hub
//
// What this file does:
//   1. Declares all global shared objects (queues, heartbeats, counters).
//   2. Calls setup() which initialises Serial, creates FreeRTOS objects,
//      and spawns all tasks.
//   3. The Arduino loop() does nothing — all work happens inside tasks.
//
// Beginner tip: In an RTOS program you hand control to the scheduler
// after setup().  You should NOT put application logic in loop().
// =============================================================================

#include <Arduino.h>
#include "config.h"
// Shared lock used by all tasks when accessing shared variables
portMUX_TYPE g_shared_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// Forward declarations for the five task functions
// (each is defined in its own .cpp file)
// ---------------------------------------------------------------------------
void sensorTask      (void* pvParameters);
void processingTask  (void* pvParameters);
void loggerTask      (void* pvParameters);
void supervisorTask  (void* pvParameters);
void faultCmdTask    (void* pvParameters);

// ---------------------------------------------------------------------------
// Global definitions — declared extern in config.h, defined once here
// ---------------------------------------------------------------------------

// FreeRTOS queue handles
QueueHandle_t g_queue_a = nullptr;   // SensorData  : Sensor → Processing
QueueHandle_t g_queue_b = nullptr;   // LogMessage  : Processing → Logger

// Heartbeat records — initialised with the current time in setup()
HeartbeatRecord g_hb_sensor     = { 0, 0, "SensorTask"     };
HeartbeatRecord g_hb_processing = { 0, 0, "ProcessingTask" };
HeartbeatRecord g_hb_logger     = { 0, 0, "LoggerTask"     };

// Fault injection flags (all false = normal operation)
volatile bool g_fault_sensor_stop    = false;
volatile bool g_fault_sensor_slow    = false;
volatile bool g_fault_invalid_data   = false;
volatile bool g_fault_queue_stress   = false;
volatile bool g_fault_queue_b_stress = false;

// Overflow / error counters
volatile uint32_t g_queue_a_overflow_count = 0;
volatile uint32_t g_queue_b_overflow_count = 0;
volatile uint32_t g_invalid_reading_count  = 0;

// ---------------------------------------------------------------------------
// setup()  —  Runs once after power-on
// ---------------------------------------------------------------------------
void setup() {
    // ---- Serial ----
    Serial.begin(SERIAL_BAUD);
    // Give the serial driver a moment to initialise in Wokwi
    vTaskDelay(MS_TO_TICKS(500));

    Serial.println(F("\n========================================"));
    Serial.println(F("  RTOS Sensor Hub — Runtime Task Health"));
    Serial.println(F("========================================"));
    Serial.println(F("Initialising FreeRTOS objects...\n"));

    // ---- Create Queue A: SensorData (Sensor → Processing) ----
    g_queue_a = xQueueCreate(QUEUE_A_DEPTH, sizeof(SensorData));
    if (g_queue_a == nullptr) {
        Serial.println(F("[FATAL] Failed to create Queue A — halting."));
        while (true) {}   // Halt; nothing else can run safely
    }

    // ---- Create Queue B: LogMessage (Processing → Logger) ----
    g_queue_b = xQueueCreate(QUEUE_B_DEPTH, sizeof(LogMessage));
    if (g_queue_b == nullptr) {
        Serial.println(F("[FATAL] Failed to create Queue B — halting."));
        while (true) {}
    }

    Serial.println(F("[INIT] Queues created OK"));

    // ---- Seed heartbeat timestamps so the supervisor does not fire
    //      immediately before the tasks have had a chance to run ----
    uint32_t now = millis();
    g_hb_sensor.last_beat_ms     = now;
    g_hb_processing.last_beat_ms = now;
    g_hb_logger.last_beat_ms     = now;

    // ---- Create tasks ----
    // xTaskCreatePinnedToCore() parameters:
    //   task function, name (debug), stack bytes, parameter, priority,
    //   handle pointer (we don't need handles here → nullptr), core id.
    //
    // Core 0: used by WiFi/BT stack (we avoid it for critical tasks).
    // Core 1: used by Arduino loop; also fine for all our tasks.

    BaseType_t rc;

    rc = xTaskCreatePinnedToCore(
        sensorTask, "SensorTask",
        STACK_SENSOR, nullptr,
        PRIORITY_SENSOR, nullptr, 1);
    if (rc != pdPASS) { Serial.println(F("[FATAL] Cannot create SensorTask")); while(true){} }

    rc = xTaskCreatePinnedToCore(
        processingTask, "ProcessingTask",
        STACK_PROCESSING, nullptr,
        PRIORITY_PROCESSING, nullptr, 1);
    if (rc != pdPASS) { Serial.println(F("[FATAL] Cannot create ProcessingTask")); while(true){} }

    rc = xTaskCreatePinnedToCore(
        loggerTask, "LoggerTask",
        STACK_LOGGER, nullptr,
        PRIORITY_LOGGER, nullptr, 1);
    if (rc != pdPASS) { Serial.println(F("[FATAL] Cannot create LoggerTask")); while(true){} }

    rc = xTaskCreatePinnedToCore(
        supervisorTask, "SupervisorTask",
        STACK_SUPERVISOR, nullptr,
        PRIORITY_SUPERVISOR, nullptr, 1);
    if (rc != pdPASS) { Serial.println(F("[FATAL] Cannot create SupervisorTask")); while(true){} }

    rc = xTaskCreatePinnedToCore(
        faultCmdTask, "FaultCmdTask",
        STACK_FAULT_CMD, nullptr,
        PRIORITY_FAULT_CMD, nullptr, 1);
    if (rc != pdPASS) { Serial.println(F("[FATAL] Cannot create FaultCmdTask")); while(true){} }

    Serial.println(F("[INIT] All tasks created. Scheduler running.\n"));
    Serial.println(F("Type HELP for available commands.\n"));
}

// ---------------------------------------------------------------------------
// loop()  —  Intentionally empty.
// The FreeRTOS scheduler now owns the CPU.  Putting blocking code here
// would starve other tasks on the same core.
// ---------------------------------------------------------------------------
void loop() {
    // Yield control back to the FreeRTOS scheduler every iteration.
    // vTaskDelay(1) suspends loop() for 1 tick, allowing other tasks to run.
    vTaskDelay(1);
}
