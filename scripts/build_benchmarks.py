#!/usr/bin/env python3

import argparse
import multiprocessing
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Tuple

ROOT_DIR = Path(__file__).resolve().parents[1]
TRANSPORTS: Dict[str, Tuple[Path, Path, str]] = {
    "fastdds": (ROOT_DIR / "apps/fastdds", ROOT_DIR / "build/fastdds", "Release"),
    "zmq": (ROOT_DIR / "apps/zmq", ROOT_DIR / "build/zmq", "Release"),
    "iceoryx": (ROOT_DIR / "apps/iceoryx", ROOT_DIR / "build/iceoryx", "Release"),
    "ros2": (ROOT_DIR / "apps/ros2", ROOT_DIR / "build/ros2", "RelWithDebInfo"),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Configure & build the benchmark transports via CMake."
    )
    parser.add_argument(
        "--transports",
        help="Comma separated list (fastdds,zmq,iceoryx,ros2). Defaults to all.",
    )
    return parser.parse_args()


def list_transports(selection: str | None) -> List[str]:
    if selection:
        transports = [item.strip() for item in selection.split(",") if item.strip()]
        invalid = [t for t in transports if t not in TRANSPORTS]
        if invalid:
            raise SystemExit(f"Unknown transports: {', '.join(invalid)}")
        return transports
    return list(TRANSPORTS.keys())


def run(cmd: List[str]) -> None:
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


def main() -> None:
    args = parse_args()
    transports = list_transports(args.transports)
    jobs = str(multiprocessing.cpu_count())

    for name in transports:
        src, build, build_type = TRANSPORTS[name]
        config_cmd = [
            "cmake",
            "-S",
            str(src),
            "-B",
            str(build),
            f"-DCMAKE_BUILD_TYPE={build_type}",
        ]
        run(config_cmd)
        build_cmd = ["cmake", "--build", str(build), "-j", jobs]
        run(build_cmd)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        print(f"Command failed with exit code {exc.returncode}", file=sys.stderr)
        sys.exit(exc.returncode)
