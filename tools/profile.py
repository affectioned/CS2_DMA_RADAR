#!/usr/bin/env python3
"""
Profile CLI — launch CS2 Radar, capture Tracy data, export LLM-friendly report.

Usage:
    python tools/profile.py [options]

Requires tracy-capture.exe and tracy-csvexport.exe in tools/tracy/ (or on PATH).
Download from https://github.com/wolfpld/tracy/releases — grab the
windows-x64 builds of both tools.
"""

import argparse
import csv
import io
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path
from datetime import datetime

SCRIPT_DIR  = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent
TRACY_DIR   = SCRIPT_DIR / "tracy"

DEFAULT_EXE = PROJECT_DIR / "bin" / "x64" / "Release" / "CS2_DMA_RADAR.exe"
ALT_EXES    = [
    PROJECT_DIR / "x64" / "Release" / "CS2_DMA_RADAR.exe",
    PROJECT_DIR / "bin" / "Release" / "CS2_DMA_RADAR.exe",
    PROJECT_DIR / "Release" / "CS2_DMA_RADAR.exe",
]


def find_tool(name: str) -> Path | None:
    local = TRACY_DIR / name
    if local.exists():
        return local
    from shutil import which
    found = which(name)
    return Path(found) if found else None


def find_exe(user_path: str | None) -> Path:
    if user_path:
        p = Path(user_path)
        if p.exists():
            return p
        sys.exit(f"Exe not found: {p}")
    if DEFAULT_EXE.exists():
        return DEFAULT_EXE
    for alt in ALT_EXES:
        if alt.exists():
            return alt
    sys.exit(
        f"Could not find CS2_DMA_RADAR.exe. Build the project first or pass --exe.\n"
        f"Searched: {DEFAULT_EXE} and {len(ALT_EXES)} alternatives."
    )


def parse_csv(raw: str) -> list[dict]:
    reader = csv.DictReader(io.StringIO(raw))
    rows = []
    for row in reader:
        rows.append(row)
    return rows


def format_ns(ns: float) -> str:
    if ns < 1_000:
        return f"{ns:.0f} ns"
    if ns < 1_000_000:
        return f"{ns / 1_000:.1f} us"
    if ns < 1_000_000_000:
        return f"{ns / 1_000_000:.2f} ms"
    return f"{ns / 1_000_000_000:.3f} s"


def generate_report(csv_text: str, duration: int, exe_path: Path) -> str:
    rows = parse_csv(csv_text)
    if not rows:
        return "# Profile Report\n\nNo zone data captured. Was the profiler connected?\n"

    zones = []
    for r in rows:
        name       = r.get("name", "?")
        src        = r.get("src_file", "")
        src_line   = r.get("src_line", "")
        call_count = int(r.get("call_count", 0) or 0)
        total_ns   = float(r.get("total_ns",  0) or 0)
        mean_ns    = float(r.get("mean_ns",   0) or 0)
        min_ns     = float(r.get("min_ns",    0) or 0)
        max_ns     = float(r.get("max_ns",    0) or 0)
        # Some builds of csvexport use different column names
        if not total_ns and "total_time_ns" in r:
            total_ns = float(r["total_time_ns"] or 0)
        if not mean_ns and "mean_time_ns" in r:
            mean_ns = float(r["mean_time_ns"] or 0)

        zones.append({
            "name": name,
            "src": f"{src}:{src_line}" if src else "",
            "calls": call_count,
            "total_ns": total_ns,
            "mean_ns": mean_ns,
            "min_ns": min_ns,
            "max_ns": max_ns,
        })

    zones.sort(key=lambda z: z["total_ns"], reverse=True)

    lines = [
        f"# Tracy Profile Report",
        f"",
        f"- **Executable**: `{exe_path.name}`",
        f"- **Duration**: {duration}s capture",
        f"- **Timestamp**: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
        f"- **Zones captured**: {len(zones)}",
        f"",
        f"## Zone Summary (sorted by total time)",
        f"",
        f"| Zone | Calls | Total | Mean | Min | Max | Source |",
        f"|------|------:|------:|-----:|----:|----:|--------|",
    ]

    for z in zones:
        lines.append(
            f"| {z['name']} "
            f"| {z['calls']:,} "
            f"| {format_ns(z['total_ns'])} "
            f"| {format_ns(z['mean_ns'])} "
            f"| {format_ns(z['min_ns'])} "
            f"| {format_ns(z['max_ns'])} "
            f"| {z['src']} |"
        )

    lines.append("")
    lines.append("## Top Hotspots (by total time)")
    lines.append("")
    for i, z in enumerate(zones[:10], 1):
        pct = ""
        total_all = sum(zz["total_ns"] for zz in zones)
        if total_all > 0:
            pct = f" ({z['total_ns'] / total_all * 100:.1f}%)"
        lines.append(
            f"{i}. **{z['name']}** — {format_ns(z['total_ns'])}{pct}, "
            f"{z['calls']:,} calls, mean {format_ns(z['mean_ns'])}, "
            f"max {format_ns(z['max_ns'])}"
        )
        if z["src"]:
            lines.append(f"   Source: `{z['src']}`")

    lines.append("")
    lines.append("## Analysis Hints")
    lines.append("")
    lines.append("Paste this report into an LLM conversation with the source code for analysis.")
    lines.append("Key questions to investigate:")
    lines.append("- Which zones dominate wall-clock time?")
    lines.append("- Are any DMA scatter read zones taking unexpectedly long (>1ms mean)?")
    lines.append("- Is the render thread (OnFrame/gameLoop) keeping up at target framerate?")
    lines.append("- Are any timer callbacks (t_*) running longer than their interval?")
    lines.append("")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Profile CS2 Radar with Tracy — capture and export LLM-friendly report."
    )
    parser.add_argument("--exe", help="Path to CS2_DMA_RADAR.exe (auto-detected if omitted)")
    parser.add_argument("--duration", "-d", type=int, default=30,
                        help="Capture duration in seconds (default: 30)")
    parser.add_argument("--output", "-o", default="profile_report.md",
                        help="Output report path (default: profile_report.md)")
    parser.add_argument("--tracy-host", default="127.0.0.1",
                        help="Tracy connection host (default: 127.0.0.1)")
    parser.add_argument("--no-launch", action="store_true",
                        help="Don't launch the exe — attach to already-running instance")
    parser.add_argument("--capture-only", action="store_true",
                        help="Save .tracy file but skip CSV export and report generation")
    args = parser.parse_args()

    capture = find_tool("tracy-capture.exe")
    csvexport = find_tool("tracy-csvexport.exe")

    if not capture:
        sys.exit(
            "tracy-capture.exe not found.\n"
            "Download from: https://github.com/wolfpld/tracy/releases\n"
            "Place in: tools/tracy/"
        )
    if not csvexport and not args.capture_only:
        sys.exit(
            "tracy-csvexport.exe not found.\n"
            "Download from: https://github.com/wolfpld/tracy/releases\n"
            "Place in: tools/tracy/"
        )

    exe_path = find_exe(args.exe)
    trace_file = Path(args.output).with_suffix(".tracy")

    # Launch the app
    app_proc = None
    if not args.no_launch:
        print(f"Launching {exe_path.name}...")
        app_proc = subprocess.Popen(
            [str(exe_path)],
            cwd=str(exe_path.parent),
        )
        time.sleep(2)

    # Start tracy-capture
    print(f"Connecting tracy-capture to {args.tracy_host}...")
    cap_cmd = [
        str(capture),
        "-a", args.tracy_host,
        "-o", str(trace_file),
        "-s", str(args.duration),
    ]
    print(f"Capturing for {args.duration}s...")
    cap_proc = subprocess.run(cap_cmd, capture_output=True, text=True)

    if cap_proc.returncode != 0:
        print(f"tracy-capture failed (exit {cap_proc.returncode}):", file=sys.stderr)
        if cap_proc.stderr:
            print(cap_proc.stderr, file=sys.stderr)
        if app_proc:
            app_proc.terminate()
        sys.exit(1)

    print(f"Capture saved: {trace_file}")

    if args.capture_only:
        if app_proc:
            print("Terminating app...")
            app_proc.terminate()
        print("Done (capture only).")
        return

    # Export to CSV
    print("Exporting to CSV...")
    csv_cmd = [str(csvexport), str(trace_file)]
    csv_proc = subprocess.run(csv_cmd, capture_output=True, text=True)

    if app_proc:
        print("Terminating app...")
        app_proc.terminate()

    if csv_proc.returncode != 0:
        print(f"tracy-csvexport failed (exit {csv_proc.returncode}):", file=sys.stderr)
        if csv_proc.stderr:
            print(csv_proc.stderr, file=sys.stderr)
        sys.exit(1)

    # Generate report
    report = generate_report(csv_proc.stdout, args.duration, exe_path)
    report_path = Path(args.output)
    report_path.write_text(report, encoding="utf-8")
    print(f"\nReport written to: {report_path}")
    print(f"Tracy file kept at: {trace_file}")
    print(f"\nPaste the report into an LLM conversation for performance analysis.")


if __name__ == "__main__":
    main()
