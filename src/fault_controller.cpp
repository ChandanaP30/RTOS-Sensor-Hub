// =============================================================================
// fault_controller.cpp  —  Serial command interface for fault injection
//
// This task reads commands from the Serial Monitor and toggles the global
// fault injection flags defined in config.h.
//
// Design notes:
//   • We use Serial.readStringUntil('\n') to get one full command per read.
//   • The string is trimmed and compared case-insensitively.
//   • All fault flags are volatile booleans shared with other tasks.
//   • This task runs at the LOWEST application priority so it cannot
//     interfere with the sensor data pipeline.
//
// Available commands and what they do:
//   HELP          — print the command list
//   STATUS        — print current fault flag states and counters
//   SENSOR_STOP   — set g_fault_sensor_stop = true
//                   The sensor task continues looping but skips both its
//                   heartbeat update and its Queue A send.  This simulates
//                   a task that is alive but no longer doing useful work.
//   SENSOR_RESUME — clear g_fault_sensor_stop
//   SENSOR_SLOW   — set g_fault_sensor_slow = true
//                   Inserts SLOW_SENSOR_EXTRA_DELAY_MS (3 s) before each
//                   sensor cycle, then resets the timing reference so the
//                   next period is a full SENSOR_PERIOD_MS (1 s) after the
//                   delay.  Effective period: ~4 s.
//   SENSOR_NORMAL — clear g_fault_sensor_slow
//   INVALID_DATA  — toggle g_fault_invalid_data (force NaN readings)
//   QUEUE_STRESS  — toggle g_fault_queue_stress (burst-fill Queue A)
//   RESET_FAULTS  — clear all fault flags at once
//
// FreeRTOS concepts:
//   • vTaskDelay() — simple blocking sleep; appropriate here because
//     this task has no strict timing requirement.
// =============================================================================

#include <Arduino.h>
#include <stdio.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Helper: push a text message to Queue B (Logger).
//
// FaultCmdTask runs at the lowest application priority (1), so it is safe to
// block here for a brief period — higher-priority tasks (Logger at priority 2)
// will preempt this task and drain Queue B during the wait.  A timeout of
// 100 ms is long enough for the Logger to free at least one slot under any
// realistic load.
//
// If Queue B is still full after 100 ms, the message is dropped silently.
// We do NOT fall back to Serial.print() because LoggerTask is the sole owner
// of Serial; a direct write here would race with the logger and produce
// garbled output on the UART.
// ---------------------------------------------------------------------------
static void cmdReport(const char* text) {
    LogMessage msg;
    msg.timestamp_ms = millis();
    snprintf(msg.text, LOG_MSG_LEN, "%s", text);
    // Block up to 100 ms; drop silently if queue remains full.
    xQueueSend(g_queue_b, &msg, MS_TO_TICKS(100));
}

// ---------------------------------------------------------------------------
// Print the help text
// ---------------------------------------------------------------------------
static void printHelp() {
    cmdReport("----------------------------------------");
    cmdReport(" AVAILABLE COMMANDS:");
    cmdReport("  HELP          - Show this help");
    cmdReport("  STATUS        - Show fault flags and counters");
    cmdReport("  SENSOR_STOP   - Stop heartbeat + data send (simulate hung task)");
    cmdReport("  SENSOR_RESUME - Resume sensor heartbeat");
    cmdReport("  SENSOR_SLOW   - Add 3s delay to sensor updates");
    cmdReport("  SENSOR_NORMAL - Remove sensor delay");
    cmdReport("  INVALID_DATA  - Force invalid (NaN) sensor readings");
    cmdReport("  QUEUE_STRESS  - Burst-fill Queue A to trigger overflow");
    cmdReport("  RESET_FAULTS  - Clear all fault injection flags");
    cmdReport("----------------------------------------");
}

// ---------------------------------------------------------------------------
// Print the current status
// ---------------------------------------------------------------------------
static void printStatus() {
    char buf[LOG_MSG_LEN];

    cmdReport("=== SYSTEM STATUS ===");

    snprintf(buf, LOG_MSG_LEN,
        "  SENSOR_STOP   : %s", g_fault_sensor_stop   ? "ON" : "off");
    cmdReport(buf);

    snprintf(buf, LOG_MSG_LEN,
        "  SENSOR_SLOW   : %s", g_fault_sensor_slow   ? "ON" : "off");
    cmdReport(buf);

    snprintf(buf, LOG_MSG_LEN,
        "  INVALID_DATA  : %s", g_fault_invalid_data  ? "ON" : "off");
    cmdReport(buf);

    snprintf(buf, LOG_MSG_LEN,
        "  QUEUE_STRESS  : %s", g_fault_queue_stress  ? "ON" : "off");
    cmdReport(buf);

    snprintf(buf, LOG_MSG_LEN,
        "  Queue A overflows : %lu", (unsigned long)g_queue_a_overflow_count);
    cmdReport(buf);

    snprintf(buf, LOG_MSG_LEN,
        "  Queue B overflows : %lu", (unsigned long)g_queue_b_overflow_count);
    cmdReport(buf);

    snprintf(buf, LOG_MSG_LEN,
        "  Invalid readings  : %lu", (unsigned long)g_invalid_reading_count);
    cmdReport(buf);

    snprintf(buf, LOG_MSG_LEN,
        "  Sensor beats      : %lu", (unsigned long)g_hb_sensor.beat_count);
    cmdReport(buf);

    snprintf(buf, LOG_MSG_LEN,
        "  Processing beats  : %lu", (unsigned long)g_hb_processing.beat_count);
    cmdReport(buf);

    snprintf(buf, LOG_MSG_LEN,
        "  Logger beats      : %lu", (unsigned long)g_hb_logger.beat_count);
    cmdReport(buf);

    uint32_t now = millis();
    snprintf(buf, LOG_MSG_LEN,
        "  Sensor last beat  : %lums ago",
        (unsigned long)(now - g_hb_sensor.last_beat_ms));
    cmdReport(buf);

    cmdReport("=====================");
}

// ---------------------------------------------------------------------------
// Process a single command string
// ---------------------------------------------------------------------------
static void processCommand(const String& cmd) {
    char buf[LOG_MSG_LEN];

    if (cmd == "HELP") {
        printHelp();

    } else if (cmd == "STATUS") {
        printStatus();

    } else if (cmd == "SENSOR_STOP") {
        g_fault_sensor_stop = true;
        cmdReport("[CMD] SENSOR_STOP activated.");
        cmdReport("      Sensor heartbeat AND queue sends are now suppressed.");
        cmdReport("      Task is alive but doing no useful work.");
        cmdReport("      Supervisor fault expected in ~3 s.");

    } else if (cmd == "SENSOR_RESUME") {
        g_fault_sensor_stop = false;
        cmdReport("[CMD] SENSOR_RESUME — sensor heartbeat restored.");

    } else if (cmd == "SENSOR_SLOW") {
        g_fault_sensor_slow = true;
        cmdReport("[CMD] SENSOR_SLOW activated — 3 s extra delay per sensor cycle.");
        cmdReport("      Effective period: ~4 s (3 s delay + 1 s normal period).");
        cmdReport("      Supervisor timeout is 3 s — fault will be reported.");

    } else if (cmd == "SENSOR_NORMAL") {
        g_fault_sensor_slow = false;
        cmdReport("[CMD] SENSOR_NORMAL — sensor delay removed.");

    } else if (cmd == "INVALID_DATA") {
        g_fault_invalid_data = !g_fault_invalid_data;  // Toggle
        snprintf(buf, LOG_MSG_LEN,
            "[CMD] INVALID_DATA %s — sensor will now produce %s readings.",
            g_fault_invalid_data ? "ON" : "OFF",
            g_fault_invalid_data ? "NaN (invalid)" : "real data");
        cmdReport(buf);

    } else if (cmd == "QUEUE_STRESS") {
        g_fault_queue_stress = !g_fault_queue_stress;  // Toggle
        snprintf(buf, LOG_MSG_LEN,
            "[CMD] QUEUE_STRESS %s — burst of %d extra items per sensor cycle.",
            g_fault_queue_stress ? "ON" : "OFF",
            (int)QUEUE_STRESS_BURST);
        cmdReport(buf);
        if (g_fault_queue_stress) {
            cmdReport("      Queue A overflows expected immediately.");
        }

    } else if (cmd == "RESET_FAULTS") {
        g_fault_sensor_stop  = false;
        g_fault_sensor_slow  = false;
        g_fault_invalid_data = false;
        g_fault_queue_stress = false;
        cmdReport("[CMD] RESET_FAULTS — all fault injection flags cleared.");
        cmdReport("      System should return to normal within one sensor cycle.");

    } else {
        snprintf(buf, LOG_MSG_LEN,
            "[CMD] Unknown command: '%s'. Type HELP for the command list.",
            cmd.c_str());
        cmdReport(buf);
    }
}

// ---------------------------------------------------------------------------
// faultCmdTask()
// ---------------------------------------------------------------------------
void faultCmdTask(void* pvParameters) {
    (void)pvParameters;

    // Brief startup delay so the boot banner prints before we start
    // polling Serial
    vTaskDelay(MS_TO_TICKS(1000));

    while (true) {
        // Check if any characters have arrived on Serial
        if (Serial.available() > 0) {
            // Read until newline (the Enter key in the Serial Monitor)
            String line = Serial.readStringUntil('\n');

            // Remove trailing whitespace (\r on Windows, spaces, etc.)
            line.trim();

            // Convert to uppercase for case-insensitive comparison
            line.toUpperCase();

            if (line.length() > 0) {
                // Echo the command back so the user can see what was received
                char echo[LOG_MSG_LEN];
                snprintf(echo, LOG_MSG_LEN, "[INPUT] Received: %s", line.c_str());
                cmdReport(echo);

                // Process it
                processCommand(line);
            }
        }

        // Poll at 50 ms — fast enough for interactive use, slow enough
        // not to waste CPU. This task has the lowest priority, so other
        // tasks preempt it freely.
        vTaskDelay(MS_TO_TICKS(50));
    }
    vTaskDelete(nullptr);
}
