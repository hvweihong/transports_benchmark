#!/usr/bin/env python3

import argparse
import datetime as dt
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence

ROOT_DIR = Path(__file__).resolve().parents[1]
RUNS_DIR = ROOT_DIR / "runs"
STREAM = "both"
DURATION = 20
HEADER_FMT = "{:<10} {:<4} {:<12} {}"
ROS_ENV_SCRIPT = ROOT_DIR / "scripts/ros_env.bash"

BINARIES: Dict[str, Path] = {
    "fastdds": ROOT_DIR / "build/fastdds/fastdds_benchmark",
    "zmq": ROOT_DIR / "build/zmq/zmq_benchmark",
    "iceoryx": ROOT_DIR / "build/iceoryx/iceoryx_benchmark",
    "ros2": ROOT_DIR / "build/ros2/ros2_benchmark",
}

_ROS_ENV_CACHE: Optional[Dict[str, str]] = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run all benchmark roles (mono/pub/sub) for the selected transports."
    )
    parser.add_argument(
        "--transports",
        help="Comma separated list (fastdds,zmq,iceoryx,ros2). "
        "Defaults to transports with existing binaries.",
    )
    return parser.parse_args()


def detect_transports(selected: Sequence[str] | None) -> List[str]:
    if selected:
        return [item.strip() for item in selected if item.strip()]
    detected = [name for name, path in BINARIES.items() if path.exists()]
    return detected


def load_ros_env() -> Dict[str, str]:
    global _ROS_ENV_CACHE
    if _ROS_ENV_CACHE is not None:
        return _ROS_ENV_CACHE
    if not ROS_ENV_SCRIPT.exists():
        raise SystemExit(f"Missing ROS env script: {ROS_ENV_SCRIPT}")

    dump_cmd = [
        "bash",
        "-lc",
        f"source \"{ROS_ENV_SCRIPT}\" >/dev/null 2>&1 && env -0",
    ]
    result = subprocess.run(
        dump_cmd,
        check=True,
        stdout=subprocess.PIPE,
        cwd=ROOT_DIR,
    )
    entries = result.stdout.split(b"\0")
    env: Dict[str, str] = {}
    for entry in entries:
        if not entry:
            continue
        key, _, value = entry.partition(b"=")
        env[key.decode()] = value.decode()
    _ROS_ENV_CACHE = env
    return env


def tee_process(cmd: List[str], log_path: Path, stream: bool, env: Optional[Dict[str, str]] = None) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log_file:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=env,
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            log_file.write(line)
            log_file.flush()
            if stream:
                sys.stdout.write(line)
                sys.stdout.flush()
        return proc.wait()


def spawn_background(cmd: List[str], log_path: Path, env: Optional[Dict[str, str]] = None) -> subprocess.Popen:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = log_path.open("w", encoding="utf-8")
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env,
    )

    def reader():
        assert proc.stdout is not None
        for line in proc.stdout:
            log_file.write(line)
            log_file.flush()
        proc.stdout.close()
        log_file.close()

    threading.Thread(target=reader, daemon=True).start()
    return proc


def summarize_metrics(log_path: Path) -> str:
    try:
        with log_path.open("r", encoding="utf-8") as log_file:
            metrics_lines = [line.strip() for line in log_file if line.startswith("[metrics]")]
    except FileNotFoundError:
        return "(no metrics captured)"

    if not metrics_lines:
        return "(no metrics captured)"

    if len(metrics_lines) > 2:
        metrics_lines = metrics_lines[1:-1]

    scalars: Dict[str, List[float]] = {}
    latency: Dict[str, List[float]] = {}
    traffic: Dict[str, List[float]] = {}

    def append(target: Dict[str, List[float]], key: str, value: float) -> None:
        target.setdefault(key, []).append(value)

    for entry in metrics_lines:
        if match := re.search(r"cpu=([0-9.]+)", entry):
            append(scalars, "cpu", float(match.group(1)))
        if match := re.search(r"rss=([0-9.]+)", entry):
            append(scalars, "rss", float(match.group(1)))
        if match := re.search(r"vmem=([0-9.]+)", entry):
            append(scalars, "vmem", float(match.group(1)))
        if match := re.search(r"latency_us\(([^)]*)\)", entry):
            for item in match.group(1).split(","):
                if "=" not in item:
                    continue
                key, value = item.split("=", 1)
                try:
                    append(latency, key.strip(), float(value.strip()))
                except ValueError:
                    continue
        if match := re.search(r"traffic\(([^)]*)\)", entry):
            for item in match.group(1).split(","):
                if "=" not in item:
                    continue
                key, value = item.split("=", 1)
                cleaned = value.strip().rstrip("hz").rstrip("HZ")
                try:
                    append(traffic, key.strip(), float(cleaned))
                except ValueError:
                    continue

    def mean(values: List[float]) -> float:
        return sum(values) / len(values)

    parts: List[str] = []
    if "cpu" in scalars:
        parts.append(f"cpu={mean(scalars['cpu']):.1f}%")
    if "rss" in scalars:
        parts.append(f"rss={mean(scalars['rss']):.1f}MB")
    if "vmem" in scalars:
        parts.append(f"vmem={mean(scalars['vmem']):.1f}MB")

    if latency:
        preferred = ["avg", "max", "samples"]
        inner: List[str] = []
        for key in preferred:
            if key in latency:
                inner.append(f"{key}={mean(latency[key]):.2f}")
        remaining = sorted(set(latency.keys()) - set(preferred))
        for key in remaining:
            inner.append(f"{key}={mean(latency[key]):.2f}")
        parts.append(f"latency_us({','.join(inner)})")

    if traffic:
        preferred = ["imu_pub", "imu_sub", "img_pub", "img_sub"]
        inner = []
        for key in preferred:
            if key in traffic:
                inner.append(f"{key}={mean(traffic[key]):.2f}hz")
        remaining = sorted(set(traffic.keys()) - set(preferred))
        for key in remaining:
            inner.append(f"{key}={mean(traffic[key]):.2f}hz")
        parts.append(f"traffic({','.join(inner)})")

    return " ".join(parts) if parts else "(no metrics captured)"


def record_result(
    summary_file,
    transport: str,
    role: str,
    exit_code: int,
    log_path: Path,
) -> None:
    metrics_line = summarize_metrics(log_path)
    status = "OK" if exit_code == 0 else f"FAIL({exit_code})"
    line = HEADER_FMT.format(transport, role, status, metrics_line)
    print(line)
    summary_file.write(line + os.linesep)
    summary_file.flush()


def main() -> None:
    args = parse_args()
    transports = detect_transports(args.transports.split(",") if args.transports else None)
    if not transports:
        print("No transports selected or built binaries not found.", file=sys.stderr)
        sys.exit(1)

    timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    cpu_name = "cpu"
    try:
        with open("/proc/cpuinfo", "r", encoding="utf-8") as cpuinfo:
            for line in cpuinfo:
                if line.lower().startswith("model name"):
                    cpu_name = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    safe_cpu = re.sub(r"[^A-Za-z0-9._-]", "_", cpu_name)
    output_dir = RUNS_DIR / f"{timestamp}_{safe_cpu}"
    output_dir.mkdir(parents=True, exist_ok=True)
    summary_path = output_dir / "summary.txt"

    with summary_path.open("w", encoding="utf-8") as summary_file:
        header = HEADER_FMT.format("Transport", "Role", "Status", "Avg metrics line")
        print(header)
        summary_file.write(header + os.linesep)

        for transport in transports:
            bin_path = BINARIES.get(transport)
            if not bin_path:
                record_result(summary_file, transport, "-", 1, Path("/dev/null"))
                continue
            if not bin_path.exists():
                line = HEADER_FMT.format(transport, "-", "MISSING", f"binary not found ({bin_path})")
                print(line)
                summary_file.write(line + os.linesep)
                summary_file.flush()
                continue
            transport_env = load_ros_env() if transport == "ros2" else None

            mono_log = output_dir / f"{transport}_mono.log"
            mono_cmd = [str(bin_path), "--role", "mono", "--stream", STREAM, "--duration", str(DURATION)]
            mono_status = tee_process(mono_cmd, mono_log, stream=True, env=transport_env)
            record_result(summary_file, transport, "mono", mono_status, mono_log)

            sub_log = output_dir / f"{transport}_sub.log"
            sub_cmd = [str(bin_path), "--role", "sub", "--stream", STREAM, "--duration", str(DURATION)]
            print(f"==> Running {transport} [sub] (log: {sub_log})")
            sub_proc = spawn_background(sub_cmd, sub_log, env=transport_env)
            time.sleep(1.0)

            pub_log = output_dir / f"{transport}_pub.log"
            pub_cmd = [str(bin_path), "--role", "pub", "--stream", STREAM, "--duration", str(DURATION)]
            pub_status = tee_process(pub_cmd, pub_log, stream=True, env=transport_env)
            record_result(summary_file, transport, "pub", pub_status, pub_log)

            sub_status = sub_proc.wait()
            record_result(summary_file, transport, "sub", sub_status, sub_log)

    print(f"Summary written to {summary_path}")


if __name__ == "__main__":
    main()
