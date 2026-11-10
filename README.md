# RTOS Sensor Hub — Runtime Task Health Monitoring

**Runtime Task Health Monitoring and Fault Injection in a FreeRTOS-Based ESP32 Sensor Hub**

---

## Table of Contents

1. [Project Introduction](#1-project-introduction)
2. [Problem Statement](#2-problem-statement)
3. [Objectives](#3-objectives)
4. [System Architecture](#4-system-architecture)
5. [Block Diagram](#5-block-diagram)
6. [Hardware and Software Requirements](#6-hardware-and-software-requirements)
7. [Pin Connections](#7-pin-connections)
8. [FreeRTOS Concepts Used](#8-freertos-concepts-used)
9. [Project File Structure](#9-project-file-structure)
10. [How to Build and Run (Step-by-Step)](#10-how-to-build-and-run-step-by-step)
11. [Serial Commands and Their Functions](#11-serial-commands-and-their-functions)
12. [Test Scenarios and Expected Output](#12-test-scenarios-and-expected-output)
13. [Performance Measurements](#13-performance-measurements)
14. [Known Limitations](#14-known-limitations)
15. [Future Improvements](#15-future-improvements)
16. [Interview Preparation Guide](#16-interview-preparation-guide)

---

## 1. Project Introduction

This project implements a **multi-task FreeRTOS application** on the ESP32 microcontroller that:

- Reads temperature and humidity from a DHT22 sensor every second.
- Processes and validates readings in a dedicated task.
- Logs all data through a single Serial-owning logger task.
- **Monitors the health of every task** using a heartbeat pattern.
- Allows you to **deliberately inject faults** via Serial commands and watch the supervisor detect and report them in real time.

The entire project runs in **Wokwi simulation** — no physical hardware required.

---

## 2. Problem Statement

In real embedded systems (medical devices, industrial controllers, automotive ECUs), a task that **silently stops responding** is more dangerous than a task that crashes visibly. Traditional error handling only catches crashes. This project demonstrates how to build a **runtime health monitoring layer** that detects silent task failures using periodic heartbeats and configurable deadlines.

---

## 3. Objectives

| # | Objective |
|---|-----------|
| 1 | Implement five independent FreeRTOS tasks with proper priorities |
| 2 | Use queues for safe inter-task data transfer |
| 3 | Implement heartbeat-based liveness monitoring |
| 4 | Detect task hangs, slow tasks, invalid data, and queue overflow |
| 5 | Provide a Serial fault injection interface for interactive testing |
| 6 | Log all events with timestamps through a single dedicated logger |
| 7 | Demonstrate recovery detection after a fault is cleared |

---

## 4. System Architecture

### Task Descriptions and Priority Rationale

| Task | Priority | Period | Role |
|------|----------|--------|------|
| `SupervisorTask` | **5** (highest) | 2000 ms | Watchdog: checks heartbeats, reports faults |
| `SensorTask` | 4 | 1000 ms | Reads DHT22, sends to Queue A |
| `ProcessingTask` | 3 | 500 ms | Validates data from Queue A, sends to Queue B |
| `LoggerTask` | 2 | 200 ms | Drains Queue B, prints to Serial |
| `FaultCmdTask` | **1** (lowest) | 50 ms poll | Reads Serial commands, sets fault flags |

**Why Supervisor has the highest priority:**
If a high-priority sensor task were to spin in an infinite loop (a common fault mode), the supervisor must still be able to preempt it and report the fault. FreeRTOS is preemptive — a higher-priority task will always interrupt a lower-priority one.

**Why FaultCmdTask has the lowest priority:**
It is an interactive command interface. A 50 ms response delay is imperceptible to a human. Giving it low priority ensures it never starves the data pipeline.

### Data Flow

```
DHT22 Sensor
     │
     ▼
 SensorTask  ──[Queue A: SensorData]──▶  ProcessingTask
                                                │
                                     [Queue B: LogMessage]
                                                │
                                                ▼
                                           LoggerTask ──▶ Serial Monitor
                                                │
                                     (all tasks also write
                                      log messages via Queue B)

 SupervisorTask  ◀── heartbeat timestamps ◀── all three worker tasks
      │
      └──[Queue B: LogMessage]──▶  LoggerTask ──▶ Serial Monitor

 FaultCmdTask  ──[volatile flags]──▶  SensorTask / all tasks
```

### Heartbeat Pattern Explained

Every worker task writes `millis()` into its `HeartbeatRecord.last_beat_ms` at the start of each cycle. The supervisor wakes up every 2 seconds and checks:

```
age = current_time − last_beat_ms
if (age > timeout_threshold) → task declared UNHEALTHY
```

This is the same principle used in industrial watchdog timers and operating system health monitoring.

---

## 5. Block Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        ESP32 (FreeRTOS)                         │
│                                                                 │
│  ┌─────────────┐    SensorData     ┌────────────────┐           │
│  │ SensorTask  │ ──── Queue A ──▶  │ ProcessingTask │           │
│  │  (Prio 4)   │                   │   (Prio 3)     │           │
│  │  HB ──────────────────────────────────────────────────────┐  │
│  └─────────────┘                   └────────────────┘        │  │
│        │                                   │                  │  │
│        │                               LogMessage             │  │
│        │                              Queue B                 │  │
│        │                                   │                  │  │
│  ┌─────────────────────────────────────────▼──────┐          │  │
│  │                   LoggerTask (Prio 2)           │          │  │
│  │              Sole owner of Serial.print()       │          │  │
│  │  HB ──────────────────────────────────────────────────────┤  │
│  └─────────────────────────────────────────────────┘          │  │
│                            │                                   │  │
│                       Serial Monitor                           │  │
│                            │                                   │  │
│  ┌─────────────────────────────────────────────────┐          │  │
│  │           SupervisorTask (Prio 5)               │          │  │
│  │  Reads: HB_sensor, HB_processing, HB_logger ◀──┘          │  │
│  │  Checks: overflow counters, invalid readings               │  │
│  │  Reports: via Queue B → Logger                             │  │
│  └─────────────────────────────────────────────────┘          │  │
│                                                               │  │
│  ┌─────────────────────────────────────────────────┐          │  │
│  │           FaultCmdTask (Prio 1)                 │          │  │
│  │  Reads Serial → sets volatile fault flags       │          │  │
│  └─────────────────────────────────────────────────┘          │  │
└─────────────────────────────────────────────────────────────────┘
         │
    GPIO 4 (DATA)
         │
    ┌────┴────┐
    │  DHT22  │  (Wokwi virtual sensor)
    └─────────┘
```

---

## 6. Hardware and Software Requirements

### Hardware (simulated in Wokwi)
| Component | Specification | Wokwi Part |
|-----------|--------------|------------|
| Microcontroller | ESP32 DevKit V1 | `wokwi-esp32-devkit-v1` |
| Sensor | DHT22 (AM2302) | `wokwi-dht22` |
| Pull-up resistor | 10 kΩ | `wokwi-resistor` |

### Software (all free)
| Tool | Purpose | Download |
|------|---------|----------|
| VS Code | IDE | https://code.visualstudio.com |
| PlatformIO Extension | Build system + library manager | VS Code Marketplace |
| Wokwi Extension for VS Code | Simulation | VS Code Marketplace |
| Git | Version control | https://git-scm.com |
| Python 3 | Log analyser script | https://python.org |

---

## 7. Pin Connections

| ESP32 Pin | DHT22 Pin | Wire Colour | Function |
|-----------|-----------|-------------|----------|
| GND | GND | Black | Ground |
| 3.3 V | VCC | Red | Power supply |
| GPIO 4 | SDA/DATA | Green | 1-Wire data |
| 3.3 V → 10 kΩ → GPIO 4 | — | — | Pull-up resistor |

> **Why a pull-up resistor?**
> The DHT22 uses an open-drain data line. Without a pull-up resistor to 3.3 V, the line floats and communication fails. In Wokwi, the virtual sensor works without it, but it is included in `diagram.json` for realism.

---

## 8. FreeRTOS Concepts Used

### 1. Tasks (`xTaskCreatePinnedToCore`)
A FreeRTOS task is an independent function that runs like a mini-program. The scheduler decides which task runs at any moment based on priority and blocking state.

### 2. Task Priorities
Higher number = higher priority. A higher-priority task **preempts** (interrupts) a lower-priority task as soon as it becomes ready to run. This is why the supervisor (priority 5) can always check heartbeats even if a lower-priority task is running.

### 3. Queues (`xQueueCreate`, `xQueueSend`, `xQueueReceive`)
A FreeRTOS queue is a thread-safe FIFO buffer. Data is copied into the queue by the sender and copied out by the receiver. No shared memory, no mutex needed — the queue API handles synchronisation internally.

```
Sender ──[xQueueSend(data)]──▶  [item][item][item]  ──[xQueueReceive()]──▶ Receiver
                                   Queue (FIFO)
```

### 4. `vTaskDelayUntil()` — Precise Periodic Execution
```cpp
TickType_t lastWakeTime = xTaskGetTickCount();
while(true) {
    // ... do work ...
    vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(1000));
    // wakes at exactly lastWakeTime + 1000ms, compensating for execution time
}
```
This is different from `vTaskDelay(1000)` which would add execution time to each period, causing drift.

### 5. Heartbeat Pattern
```
Task writes:    g_hb_sensor.last_beat_ms = millis();  // "I'm alive"
Supervisor reads: age = millis() - g_hb_sensor.last_beat_ms;
                  if (age > 3000) → FAULT detected
```

### 6. `volatile` Keyword
When one task writes a flag and another reads it, the compiler might optimise by caching the value in a CPU register. `volatile` tells the compiler: "always read this from memory, never cache it." This prevents subtle bugs where a task never sees a flag change.

### 7. Queue Overflow Detection
`xQueueSend(queue, data, 0)` with timeout=0 returns `pdFALSE` immediately if the queue is full. We count these failures instead of blocking, which prevents priority inversion.

---

## 9. Project File Structure

```
RTOS_Sensor_Hub/
│
├── platformio.ini          ← Build configuration, library deps, board settings
├── diagram.json            ← Wokwi circuit definition (ESP32 + DHT22 + resistor)
├── wokwi.toml              ← Tells Wokwi where to find the compiled firmware
├── README.md               ← This file
├── log_analyser.py         ← Python script to analyse Serial Monitor logs
│
├── include/
│   └── config.h            ← All constants, structs, extern declarations
│
└── src/
    ├── main.cpp            ← setup(), loop(), global object definitions
    ├── sensor_task.cpp     ← DHT22 reading, Queue A sender, heartbeat
    ├── processing_task.cpp ← Data validation, Queue B sender, heartbeat
    ├── logger_task.cpp     ← Serial output, Queue B receiver, heartbeat
    ├── supervisor_task.cpp ← Heartbeat checker, fault reporter
    └── fault_controller.cpp← Serial command handler, fault flags
```

---

## 10. How to Build and Run (Step-by-Step)

### Method A: VS Code + PlatformIO + Wokwi (Recommended)

#### Prerequisites
1. Install VS Code: https://code.visualstudio.com
2. Open VS Code → Extensions (Ctrl+Shift+X) → Search and install:
   - **PlatformIO IDE** (by PlatformIO)
   - **Wokwi Simulator** (by Wokwi)
3. Get a free Wokwi licence for VS Code: https://wokwi.com/vscode

#### Build Steps
```bash
# 1. Open the project folder in VS Code
File → Open Folder → Select RTOS_Sensor_Hub/

# 2. Build the firmware
# Click the ✓ (Build) button in the PlatformIO toolbar at the bottom
# OR press: Ctrl+Shift+P → "PlatformIO: Build"
# OR in terminal:
pio run

# 3. Run in Wokwi
# Press F1 → "Wokwi: Start Simulator"
# OR click the play button in the Wokwi panel
```

#### Using the Serial Monitor in Wokwi
- The Wokwi simulator has a built-in Serial Monitor.
- Type commands directly into the input box at the bottom of the Serial Monitor.
- Press Enter to send.

### Method B: Wokwi Online (wokwi.com)

1. Go to https://wokwi.com
2. Create a new project → Select "ESP32"
3. Replace `diagram.json` with the contents of this project's `diagram.json`
4. Add a new file `main.cpp` and paste the content, repeat for all `.cpp` files
5. Add `config.h` in a new header file
6. Add the DHT library in `libraries.txt`
7. Click the green ▶ Run button

> **Note:** The online Wokwi editor does not support multi-file PlatformIO projects natively. You may need to combine all source files or use the VS Code Wokwi extension.

### Method C: Command Line

```bash
# Install PlatformIO
pip install platformio

# Navigate to project
cd RTOS_Sensor_Hub

# Build
pio run

# Upload to real hardware (optional)
pio run --target upload

# Open Serial Monitor
pio device monitor --baud 115200
```

---

## 11. Serial Commands and Their Functions

Open the Serial Monitor at **115200 baud**. Type any command and press Enter.

| Command | What It Does | Expected Result |
|---------|-------------|-----------------|
| `HELP` | Shows all commands | Command list printed |
| `STATUS` | Shows current fault flags and counters | Current system state printed |
| `SENSOR_STOP` | Freezes the sensor heartbeat | Supervisor detects fault within ~3 s |
| `SENSOR_RESUME` | Restores sensor heartbeat | Supervisor reports recovery |
| `SENSOR_SLOW` | Adds 3 s delay to sensor task | Sensor period becomes ~4 s, fault detected |
| `SENSOR_NORMAL` | Removes the delay | Normal 1 s period resumes |
| `INVALID_DATA` | Toggles NaN sensor readings | Processing Task reports DATA-ERR |
| `QUEUE_STRESS` | Toggles burst-filling of Queue A | Queue overflows reported immediately |
| `RESET_FAULTS` | Clears all fault flags | System returns to normal operation |

---

## 12. Test Scenarios and Expected Output

### Test 1: Normal Operation
**Procedure:** Just start the simulator. Wait 10 seconds.

**Expected output:**
```
========================================
  RTOS Sensor Hub — Runtime Task Health
========================================
Initialising FreeRTOS objects...

[INIT] Queues created OK
[INIT] All tasks created. Scheduler running.

Type HELP for available commands.

[    1012ms] [SENSOR]   T=25.5°C  H=60.0%  | t=1010ms
[    1515ms] [SENSOR]   T=25.5°C  H=60.0%  | t=1512ms
[    2000ms] [SUPER] t=2000ms | Sensor:OK Processing:OK Logger:OK | QA_OVF:0 QB_OVF:0 INV:0
[    2017ms] [SENSOR]   T=25.5°C  H=60.0%  | t=2015ms
...
```

### Test 2: Sensor Task Stops Reporting

**Procedure:** Type `SENSOR_STOP` in Serial Monitor.

**Expected output (within 3–5 seconds):**
```
[   5200ms] [INPUT] Received: SENSOR_STOP
[   5201ms] [CMD] SENSOR_STOP activated — sensor heartbeat will freeze.
[   5202ms]       Watch for supervisor fault report in ~3 seconds.
[   8000ms] [SUPER] t=8000ms | Sensor:FAIL Processing:OK Logger:OK | ...
[   8001ms] [FAULT] *** SensorTask UNRESPONSIVE! Last beat 3100ms ago (timeout=3000ms) ***
```

**What to observe:** Only the Sensor heartbeat stops. Processing and Logger continue running normally. Sensor readings stop appearing (Queue A empties), but the supervisor summary still prints every 2 seconds.

**Type `SENSOR_RESUME` to clear:**
```
[  12000ms] [RECOVERY] SensorTask is healthy again. Beat count=12
```

### Test 3: Sensor Task Becomes Slow

**Procedure:** Type `SENSOR_SLOW`.

**Expected output:**
```
[CMD] SENSOR_SLOW activated — 3s extra delay added to sensor.
      Sensor period becomes ~4s; timeout is 3s, fault expected.
[FAULT] *** SensorTask UNRESPONSIVE! Last beat 3200ms ago (timeout=3000ms) ***
```

**Key point:** The task is not dead — it's just slow. After it completes its delayed cycle and beats the heartbeat, the supervisor reports recovery:
```
[RECOVERY] SensorTask is healthy again.
```

**Type `SENSOR_NORMAL` to clear.**

### Test 4: Invalid Sensor Readings

**Procedure:** Type `INVALID_DATA`.

**Expected output:**
```
[CMD] INVALID_DATA ON — sensor will now produce NaN (invalid) readings.
[DATA-ERR] Invalid reading at 15200ms | Temp=nan | Hum=nan
[WARN] Invalid sensor readings detected. Total: 1
[DATA-ERR] Invalid reading at 16205ms | Temp=nan | Hum=nan
[WARN] Invalid sensor readings detected. Total: 2
```

**Key point:** The supervisor distinguishes between "task is not responding" (heartbeat failure) and "task is running but producing bad data" (invalid reading counter). The sensor task IS alive and updating its heartbeat — only the data quality is bad.

**Type `INVALID_DATA` again to toggle off.**

### Test 5: Queue Overflow

**Procedure:** Type `QUEUE_STRESS`.

**Expected output (within 1 second):**
```
[CMD] QUEUE_STRESS ON — burst of 20 extra items per sensor cycle.
      Queue A overflows expected immediately.
[FAULT] Queue A overflow! Total lost packets: 16
[FAULT] Queue A overflow! Total lost packets: 36
```

**Type `QUEUE_STRESS` again to toggle off.**

### Test 6: Recovery

**Procedure:** After any fault test, send `RESET_FAULTS`.

**Expected output:**
```
[CMD] RESET_FAULTS — all fault injection flags cleared.
      System should return to normal within one sensor cycle.
[RECOVERY] SensorTask is healthy again. Beat count=42
[SUPER] t=30000ms | Sensor:OK Processing:OK Logger:OK | QA_OVF:16 QB_OVF:0 INV:5
```

> Note: Overflow counters are cumulative and do not reset with `RESET_FAULTS`. They record all events since boot. This is realistic — in production systems, you would typically send counters to a telemetry server.

---

## 13. Performance Measurements

All measurements are based on actual program state variables, not hardcoded values.

| Metric | How Measured | Typical Value (Wokwi) |
|--------|-------------|----------------------|
| Sensor interval | `g_hb_sensor.beat_count` over time | ~1000 ms ±50 ms |
| Fault detection time (SENSOR_STOP) | Time from command to `[FAULT]` line | ~3000–5000 ms |
| Recovery detection time | Time from `SENSOR_RESUME` to `[RECOVERY]` | ~2000 ms (one supervisor cycle) |
| Queue A overflow onset (QUEUE_STRESS) | First overflow within | <1000 ms |
| Invalid reading count | `g_invalid_reading_count` | Exact count |

### Measurement Limitations in Wokwi

1. **Simulated time is not real time.** Wokwi runs faster or slower than a real ESP32 depending on your PC load. `millis()` counts simulated ticks, not wall-clock time.
2. **DHT22 timing is emulated.** The virtual sensor always returns its configured values instantly, unlike a real DHT22 that takes 2 ms per read and can fail.
3. **Serial I/O latency.** In Wokwi, `Serial.println()` is very fast. On real hardware, UART transmission adds real time proportional to string length and baud rate.
4. **Scheduling jitter.** FreeRTOS tick resolution is 1 ms (configurable). Tasks cannot wake up more precisely than 1 tick. Wokwi may add additional jitter.

**Use the `log_analyser.py` script** to calculate accurate intervals, detection latency, and statistics from your actual Serial Monitor output.

---

## 14. Known Limitations

| Limitation | Explanation | Workaround |
|-----------|-------------|------------|
| No mutex around Serial | Only LoggerTask calls `Serial.println()` for normal logs, so no race condition. The supervisor uses `Serial.print()` as a last-resort fallback only when Queue B is full. | Use Queue B for all logging |
| `volatile` not `atomic` | On ESP32 (Xtensa LX6, 32-bit), 32-bit variable reads/writes are atomic by hardware. For multi-word reads, use a mutex or `portENTER_CRITICAL()`. | Acceptable for this project |
| No task restart on failure | The supervisor detects but cannot restart a failed task (no handle stored). | Store task handles and call `vTaskDelete()` + `xTaskCreate()` |
| Wokwi-only testing | Cannot verify real hardware timing or actual DHT22 failure modes. | Test on real hardware with an actual DHT22 |
| Overflow counters never reset | Counts accumulate since boot. | Add a `RESET_COUNTERS` command |

---

## 15. Future Improvements

1. **Task restart on fault:** Store `TaskHandle_t` for each task; supervisor can call `vTaskDelete()` and recreate the task on fault detection.
2. **Hardware watchdog timer (IWDT):** Use ESP32's hardware watchdog. The supervisor feeds it; if the supervisor itself fails, the chip resets.
3. **Non-volatile logging:** Write fault events to ESP32 flash (NVS or SPIFFS) so they survive a reboot.
4. **LED indicators:** GPIO outputs to show system status visually (green = OK, red = fault).
5. **MQTT telemetry:** Send heartbeat and fault data to an MQTT broker when WiFi is available.
6. **FreeRTOS software timers:** Replace polling with event-driven timers for lower CPU usage.
7. **Stack high-water mark monitoring:** Use `uxTaskGetStackHighWaterMark()` to detect stack overflows before they crash the system.

---

## 16. Interview Preparation Guide

### Core Concepts to Explain

**Q: What is RTOS and why use it on a microcontroller?**
> A Real-Time Operating System manages multiple tasks with guaranteed timing. On a bare-metal ESP32 with a single main loop, one slow operation (like a network call) blocks everything else. FreeRTOS runs tasks concurrently, giving each a time slice based on priority, so the sensor still reads every second even if the logger is busy printing.

**Q: Explain the heartbeat pattern.**
> Every task writes the current time (`millis()`) into a shared variable at the start of each cycle. A separate supervisor task periodically reads these timestamps. If the gap between "now" and "last heartbeat" exceeds a threshold, the task has missed its expected update — it may be hung or crashed. This is similar to a watchdog timer, but software-based and per-task.

**Q: Why does the supervisor have the highest priority?**
> FreeRTOS is preemptive. If a buggy task runs an infinite loop without yielding, it would monopolize the CPU at its priority level. By giving the supervisor a higher priority, it can always preempt lower-priority tasks to check their heartbeats and report faults, even if those tasks are stuck.

**Q: What is the difference between `vTaskDelay()` and `vTaskDelayUntil()`?**
> `vTaskDelay(1000)` sleeps for 1000 ms *from now*. If the task body took 50 ms to execute, the actual period becomes 1050 ms and drifts over time. `vTaskDelayUntil(&lastWakeTime, 1000)` wakes at exactly `lastWakeTime + 1000`, compensating for execution time and maintaining a constant period.

**Q: Why do you use `volatile` for the fault flags?**
> Without `volatile`, the compiler may read the flag once and cache it in a CPU register, never re-reading from memory. If another task modifies the flag in memory, the first task would never see the change. `volatile` forces every read to go to actual memory, ensuring all tasks see the latest value.

**Q: What happens if Queue A is full when the sensor tries to send?**
> `xQueueSend(queue, data, 0)` with a timeout of 0 returns `pdFALSE` immediately instead of blocking. We increment `g_queue_a_overflow_count` and continue. This is non-blocking queue overflow handling — the task does not get stuck waiting, and the supervisor will report the overflow count.

**Q: How is this different from a simple watchdog timer?**
> A hardware watchdog resets the entire CPU if not fed. This design monitors each task independently — it can detect which specific task failed and report it while keeping other tasks running. Recovery is detected and reported without a full system reset.

---

## Licence

This project is released for educational purposes. Free to use, modify, and share.
