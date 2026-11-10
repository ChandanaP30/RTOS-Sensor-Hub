#!/usr/bin/env python3
"""
log_analyser.py  —  Offline analysis of RTOS Sensor Hub Serial logs
====================================================================

Usage:
    1. In Wokwi or VS Code Serial Monitor, copy all output and save it
       as  serial_log.txt  in the same folder as this script.
    2. Run:  python log_analyser.py
    3. Optionally specify a file: python log_analyser.py mylog.txt

What it reports:
    - Total sensor readings (valid vs invalid)
    - Number of fault events detected by the supervisor
    - Number of recovery events
    - Queue overflow counts
    - Min / max / average temperature and humidity
    - Fault detection latency (time from last OK to first FAULT line)
    - Task heartbeat statistics
"""

import sys
import re
from pathlib import Path
from collections import defaultdict

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
DEFAULT_LOG_FILE = "serial_log.txt"

# Regex patterns matched against each log line
PATTERNS = {
    "sensor_reading": re.compile(
        r"\[SENSOR\]\s+T=([\-\d.]+)°?C\s+H=([\d.]+)%.*t=(\d+)ms"
    ),
    "data_error": re.compile(r"\[DATA-ERR\]"),
    "fault": re.compile(r"\[FAULT\]"),
    "recovery": re.compile(r"\[RECOVERY\]"),
    "supervisor": re.compile(
        r"\[SUPER\].*t=(\d+)ms.*Sensor:(\w+).*Processing:(\w+).*Logger:(\w+)"
        r".*QA_OVF:(\d+).*QB_OVF:(\d+).*INV:(\d+)"
    ),
    "timestamp": re.compile(r"\[\s*(\d+)ms\]"),
    "cmd_input": re.compile(r"\[INPUT\] Received: (.+)"),
    "unresponsive": re.compile(
        r"\[FAULT\] \*\*\* (\w+) UNRESPONSIVE! Last beat (\d+)ms ago"
    ),
    "qa_overflow": re.compile(r"Queue A overflow! Total lost packets: (\d+)"),
    "qb_overflow": re.compile(r"Queue B overflow! Total lost log messages: (\d+)"),
}


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------
def parse_log(filepath: Path) -> dict:
    results = {
        "temperatures": [],
        "humidities": [],
        "sensor_timestamps": [],
        "data_errors": 0,
        "fault_lines": [],
        "recovery_lines": [],
        "supervisor_rows": [],
        "commands_sent": [],
        "unresponsive_events": [],
        "qa_overflow_max": 0,
        "qb_overflow_max": 0,
        "raw_lines": [],
    }

    with open(filepath, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    results["raw_lines"] = lines

    for line in lines:
        line = line.strip()
        if not line:
            continue

        # Sensor reading
        m = PATTERNS["sensor_reading"].search(line)
        if m:
            results["temperatures"].append(float(m.group(1)))
            results["humidities"].append(float(m.group(2)))
            results["sensor_timestamps"].append(int(m.group(3)))
            continue

        # Data error
        if PATTERNS["data_error"].search(line):
            results["data_errors"] += 1
            continue

        # Fault event
        if PATTERNS["fault"].search(line):
            results["fault_lines"].append(line)

        # Recovery event
        if PATTERNS["recovery"].search(line):
            results["recovery_lines"].append(line)

        # Supervisor summary
        m = PATTERNS["supervisor"].search(line)
        if m:
            results["supervisor_rows"].append({
                "t": int(m.group(1)),
                "sensor": m.group(2),
                "processing": m.group(3),
                "logger": m.group(4),
                "qa_ovf": int(m.group(5)),
                "qb_ovf": int(m.group(6)),
                "inv": int(m.group(7)),
            })
            results["qa_overflow_max"] = max(results["qa_overflow_max"], int(m.group(5)))
            results["qb_overflow_max"] = max(results["qb_overflow_max"], int(m.group(6)))
            continue

        # Commands
        m = PATTERNS["cmd_input"].search(line)
        if m:
            results["commands_sent"].append(m.group(1))

        # Unresponsive events
        m = PATTERNS["unresponsive"].search(line)
        if m:
            results["unresponsive_events"].append({
                "task": m.group(1),
                "age_ms": int(m.group(2)),
                "line": line,
            })

    return results


# ---------------------------------------------------------------------------
# Analysis helpers
# ---------------------------------------------------------------------------
def safe_avg(lst):
    return sum(lst) / len(lst) if lst else float("nan")


def fault_detection_latency(results: dict) -> str:
    """
    Estimate the latency between a task stopping its heartbeat (SENSOR_STOP
    command received) and the first FAULT line appearing.
    """
    # Find timestamp of SENSOR_STOP command
    stop_cmd_idx = None
    for i, line in enumerate(results["raw_lines"]):
        if "SENSOR_STOP" in line and "INPUT" in line:
            stop_cmd_idx = i
            break

    if stop_cmd_idx is None:
        return "No SENSOR_STOP command found in log."

    # Find first UNRESPONSIVE fault line after the command
    fault_idx = None
    for i in range(stop_cmd_idx, len(results["raw_lines"])):
        if "UNRESPONSIVE" in results["raw_lines"][i]:
            fault_idx = i
            break

    if fault_idx is None:
        return "No UNRESPONSIVE fault found after SENSOR_STOP."

    # Extract millisecond timestamps from both lines
    def extract_ms(line):
        m = PATTERNS["timestamp"].search(line)
        return int(m.group(1)) if m else None

    t_stop = extract_ms(results["raw_lines"][stop_cmd_idx])
    t_fault = extract_ms(results["raw_lines"][fault_idx])

    if t_stop is not None and t_fault is not None:
        return f"{t_fault - t_stop} ms  (from SENSOR_STOP at {t_stop}ms to fault at {t_fault}ms)"
    return "Timestamps not found in relevant lines."


# ---------------------------------------------------------------------------
# Report printing
# ---------------------------------------------------------------------------
def print_report(results: dict):
    sep = "=" * 60

    print(f"\n{sep}")
    print("  RTOS SENSOR HUB — Serial Log Analysis Report")
    print(sep)

    # --- Sensor data statistics ---
    temps = results["temperatures"]
    hums  = results["humidities"]
    print(f"\n[ Sensor Data ]")
    print(f"  Valid readings    : {len(temps)}")
    print(f"  Invalid readings  : {results['data_errors']}")
    if temps:
        print(f"  Temperature  min  : {min(temps):.1f} °C")
        print(f"  Temperature  max  : {max(temps):.1f} °C")
        print(f"  Temperature  avg  : {safe_avg(temps):.2f} °C")
        print(f"  Humidity     min  : {min(hums):.1f} %")
        print(f"  Humidity     max  : {max(hums):.1f} %")
        print(f"  Humidity     avg  : {safe_avg(hums):.2f} %")

    # --- Interval analysis ---
    if len(results["sensor_timestamps"]) >= 2:
        ts = results["sensor_timestamps"]
        intervals = [ts[i+1] - ts[i] for i in range(len(ts)-1)]
        print(f"\n[ Sensor Intervals ]")
        print(f"  Count             : {len(intervals)}")
        print(f"  Min interval      : {min(intervals)} ms")
        print(f"  Max interval      : {max(intervals)} ms")
        print(f"  Avg interval      : {safe_avg(intervals):.1f} ms")
        print(f"  Expected          : {1000} ms")

    # --- Fault events ---
    print(f"\n[ Fault Events ]")
    print(f"  Fault lines total : {len(results['fault_lines'])}")
    print(f"  Recovery events   : {len(results['recovery_lines'])}")
    if results["unresponsive_events"]:
        print(f"  Unresponsive tasks:")
        for ev in results["unresponsive_events"]:
            print(f"    - {ev['task']} (beat age: {ev['age_ms']} ms)")

    # --- Fault detection latency ---
    print(f"\n[ Fault Detection Latency ]")
    print(f"  SENSOR_STOP → fault detected: {fault_detection_latency(results)}")

    # --- Queue statistics ---
    print(f"\n[ Queue Statistics ]")
    print(f"  Queue A max overflows : {results['qa_overflow_max']}")
    print(f"  Queue B max overflows : {results['qb_overflow_max']}")

    # --- Commands ---
    if results["commands_sent"]:
        print(f"\n[ Commands Received ]")
        for cmd in results["commands_sent"]:
            print(f"    > {cmd}")

    # --- Supervisor summary ---
    rows = results["supervisor_rows"]
    if rows:
        print(f"\n[ Supervisor Cycles ]")
        print(f"  Total cycles        : {len(rows)}")
        ok_counts = defaultdict(int)
        for r in rows:
            for key in ("sensor", "processing", "logger"):
                if r[key] == "OK":
                    ok_counts[key] += 1
        for key in ("sensor", "processing", "logger"):
            pct = 100 * ok_counts[key] // len(rows)
            print(f"  {key.capitalize():12s} OK : {ok_counts[key]}/{len(rows)} ({pct}%)")

    print(f"\n{sep}\n")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main():
    log_file = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(DEFAULT_LOG_FILE)

    if not log_file.exists():
        print(f"[ERROR] Log file not found: {log_file}")
        print("Save your Wokwi/Serial Monitor output as 'serial_log.txt' first.")
        sys.exit(1)

    print(f"Parsing: {log_file.resolve()}")
    results = parse_log(log_file)
    print_report(results)


if __name__ == "__main__":
    main()
