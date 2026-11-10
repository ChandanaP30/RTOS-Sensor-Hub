#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// config.h  —  Central configuration for the RTOS Sensor Hub project
//
// All tuneable constants live here so you can adjust timing and behaviour
// without digging through every source file.
//
// Beginner tip: "constexpr" means the value is calculated at compile time
// and cannot change at runtime.  It is safer than #define because it
// is type-checked by the compiler.
// =============================================================================

#include <Arduino.h>   // TickType_t, portTICK_PERIOD_MS

// ---------------------------------------------------------------------------
// Hardware pin definitions
// ---------------------------------------------------------------------------
constexpr uint8_t DHT_PIN  = 4;    // GPIO4 — data line of the DHT22 sensor
constexpr uint8_t DHT_TYPE = 22;   // 22 = DHT22 (AM2302)

// ---------------------------------------------------------------------------
// Serial communication
// ---------------------------------------------------------------------------
constexpr uint32_t SERIAL_BAUD = 115200;   // bits per second

// ---------------------------------------------------------------------------
// FreeRTOS queue sizes
// The queue holds structs, not raw bytes.
// A depth of 5 is enough for normal operation; we stress this in Test 5.
// ---------------------------------------------------------------------------
constexpr UBaseType_t QUEUE_A_DEPTH = 5;   // Sensor → Processing
constexpr UBaseType_t QUEUE_B_DEPTH = 10;  // Processing → Logger (larger:
                                           //   logger does Serial I/O which
                                           //   is slower than processing)

// ---------------------------------------------------------------------------
// Task stack sizes (in bytes, NOT words, because ESP32 uses byte units)
// 4096 bytes is a safe default; sensor/logger need a bit more for
// the DHT library and printf formatting.
// ---------------------------------------------------------------------------
constexpr uint32_t STACK_SENSOR     = 4096;
constexpr uint32_t STACK_PROCESSING = 3072;
constexpr uint32_t STACK_LOGGER     = 4096;
constexpr uint32_t STACK_SUPERVISOR = 3072;
constexpr uint32_t STACK_FAULT_CMD  = 3072;

// ---------------------------------------------------------------------------
// Task priorities
//
// Priority design rationale (higher number = higher priority in FreeRTOS):
//
//  1  FaultCmdTask   — reads Serial input; lowest priority so it never
//                      starves sensor data processing.
//  2  LoggerTask     — produces output; must run often but can wait briefly.
//  3  ProcessingTask — transforms data; runs between sensor and logger.
//  4  SensorTask     — acquires data; needs to run on time each second.
//  5  SupervisorTask — watchdog; highest priority so a stalled task does
//                      NOT prevent the supervisor from detecting the fault.
//
// All are below the FreeRTOS idle task priority (0) is impossible, so
// priority 1 is the lowest usable level in Arduino-ESP32.
// ---------------------------------------------------------------------------
constexpr UBaseType_t PRIORITY_FAULT_CMD  = 1;
constexpr UBaseType_t PRIORITY_LOGGER     = 2;
constexpr UBaseType_t PRIORITY_PROCESSING = 3;
constexpr UBaseType_t PRIORITY_SENSOR     = 4;
constexpr UBaseType_t PRIORITY_SUPERVISOR = 5;

// ---------------------------------------------------------------------------
// Task execution periods (milliseconds)
// ---------------------------------------------------------------------------
constexpr uint32_t SENSOR_PERIOD_MS     = 1000;  // Sensor reads every 1 s
constexpr uint32_t PROCESSING_PERIOD_MS = 500;   // Processing checks queue every 500 ms
constexpr uint32_t LOGGER_PERIOD_MS     = 200;   // Logger drains queue every 200 ms
constexpr uint32_t SUPERVISOR_PERIOD_MS = 2000;  // Supervisor checks every 2 s

// ---------------------------------------------------------------------------
// Health supervisor thresholds
//
// A task is declared UNHEALTHY if it has not updated its heartbeat within
// this many milliseconds.  We give a generous margin (2× the normal period)
// to avoid false alarms from normal scheduling jitter.
//
// Separate threshold for SLOW mode (SENSOR_SLOW command adds 3 s delay).
// ---------------------------------------------------------------------------
constexpr uint32_t SENSOR_HEARTBEAT_TIMEOUT_MS     = 3000;  // 3× normal period
constexpr uint32_t PROCESSING_HEARTBEAT_TIMEOUT_MS = 2000;  // 4× normal period
constexpr uint32_t LOGGER_HEARTBEAT_TIMEOUT_MS     = 1000;  // 5× normal period

// ---------------------------------------------------------------------------
// Fault injection parameters
// ---------------------------------------------------------------------------
constexpr uint32_t SLOW_SENSOR_EXTRA_DELAY_MS = 3000; // Added delay when SENSOR_SLOW
constexpr uint32_t QUEUE_STRESS_BURST         = 20;   // Items pushed rapidly during QUEUE_STRESS

// ---------------------------------------------------------------------------
// Shared data structure: one sensor reading passed between tasks
//
// Using a plain struct keeps things simple.  The Queue API copies the
// entire struct by value, so there is no need for dynamic memory.
// ---------------------------------------------------------------------------
struct SensorData {
    float    temperature;    // degrees Celsius  (NaN if invalid)
    float    humidity;       // percent RH       (NaN if invalid)
    uint32_t timestamp_ms;   // millis() when the reading was taken
    bool     valid;          // false if sensor returned NaN
};

// ---------------------------------------------------------------------------
// Shared data structure: a log message passed from Processing → Logger
// ---------------------------------------------------------------------------
constexpr size_t LOG_MSG_LEN = 160;  // Maximum characters per log message

struct LogMessage {
    char     text[LOG_MSG_LEN];  // Null-terminated string
    uint32_t timestamp_ms;       // When this message was created
};

// ---------------------------------------------------------------------------
// Heartbeat record — one per monitored task
// volatile: the compiler must not cache this in a register because it is
// written by one task and read by another.
// ---------------------------------------------------------------------------
struct HeartbeatRecord {
    volatile uint32_t last_beat_ms;   // millis() of the most recent heartbeat
    volatile uint32_t beat_count;     // total beats since boot
    const char*       task_name;      // human-readable name for logs
};

// ---------------------------------------------------------------------------
// Global fault injection flags
// Declared extern here; defined once in main.cpp.
// volatile: written by FaultCmdTask, read by SensorTask.
// ---------------------------------------------------------------------------
extern volatile bool g_fault_sensor_stop;    // stop sensor heartbeat
extern volatile bool g_fault_sensor_slow;    // add extra delay to sensor
extern volatile bool g_fault_invalid_data;   // force NaN readings
extern volatile bool g_fault_queue_stress;   // burst-fill the queue

// ---------------------------------------------------------------------------
// Global heartbeat records (defined in main.cpp, used by all tasks)
// ---------------------------------------------------------------------------
extern HeartbeatRecord g_hb_sensor;
extern HeartbeatRecord g_hb_processing;
extern HeartbeatRecord g_hb_logger;

// ---------------------------------------------------------------------------
// FreeRTOS queue handles (defined in main.cpp)
// QueueHandle_t is a pointer type managed by FreeRTOS internally.
// ---------------------------------------------------------------------------
extern QueueHandle_t g_queue_a;   // SensorData:   Sensor → Processing
extern QueueHandle_t g_queue_b;   // LogMessage:   Processing → Logger

// ---------------------------------------------------------------------------
// Global counters (atomic-safe on ESP32 for 32-bit types read/written once)
// ---------------------------------------------------------------------------
extern volatile uint32_t g_queue_a_overflow_count;
extern volatile uint32_t g_queue_b_overflow_count;
extern volatile uint32_t g_invalid_reading_count;

// ---------------------------------------------------------------------------
// Helper macro: convert milliseconds to FreeRTOS ticks
// pdMS_TO_TICKS is defined in FreeRTOS and handles the conversion safely.
// ---------------------------------------------------------------------------
#define MS_TO_TICKS(ms)  pdMS_TO_TICKS(ms)

#endif // CONFIG_H
