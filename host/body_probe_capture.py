#!/usr/bin/env python3
"""Trigger the Cardputer body probe, save its CSV and print a quick SNR check."""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover - depends on the host setup
    raise SystemExit(
        "pyserial is required; run this with PlatformIO's Python environment"
    ) from exc


BEGIN = "# BODY_PROBE_BEGIN"
END = "# BODY_PROBE_END"
VID = 0x303A
PID = 0x1001


@dataclass(frozen=True)
class Sample:
    index: int
    x: int
    y: int
    z: int


def discover_port() -> str:
    matches = [
        port.device
        for port in list_ports.comports()
        if port.vid == VID and port.pid == PID
    ]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise SystemExit(
            "Cardputer-ADV not found; pass its serial device with --port"
        )
    raise SystemExit(
        "More than one Espressif USB device found; choose one with --port: "
        + ", ".join(matches)
    )


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def read_capture(port: serial.Serial, command: bytes, timeout: float) -> list[str]:
    port.reset_input_buffer()
    port.write(command + b"\n")
    port.flush()

    deadline = time.monotonic() + timeout
    lines: list[str] = []
    recent: list[str] = []
    collecting = False
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            continue
        recent.append(line)
        recent = recent[-8:]
        if line.startswith("BODY_PROBE_ERROR"):
            raise RuntimeError(f"device rejected the capture: {line}")
        if line.startswith(BEGIN):
            collecting = True
        if collecting:
            lines.append(line)
        if collecting and line.startswith(END):
            return lines

    diagnostic = "\n".join(recent) if recent else "(no serial output)"
    raise TimeoutError(f"probe capture timed out; last device output:\n{diagnostic}")


def parse_samples(lines: Iterable[str]) -> list[Sample]:
    data_lines = [line for line in lines if not line.startswith("#")]
    if not data_lines or data_lines[0] != "sample,x,y,z":
        raise ValueError("capture has no valid sample header")

    samples: list[Sample] = []
    for expected_index, row in enumerate(csv.reader(data_lines[1:])):
        if len(row) != 4:
            raise ValueError(f"malformed CSV row {expected_index}: {row!r}")
        try:
            sample = Sample(*(int(value) for value in row))
        except ValueError as exc:
            raise ValueError(
                f"non-integer CSV row {expected_index}: {row!r}"
            ) from exc
        if sample.index != expected_index:
            raise ValueError(
                f"sample index discontinuity: got {sample.index}, "
                f"expected {expected_index}"
            )
        samples.append(sample)
    return samples


def vector_rms(samples: list[Sample], center: tuple[float, float, float]) -> float:
    if not samples:
        return float("nan")
    cx, cy, cz = center
    energy = sum(
        (sample.x - cx) ** 2 + (sample.y - cy) ** 2 + (sample.z - cz) ** 2
        for sample in samples
    )
    return math.sqrt(energy / len(samples))


def mean_axes(samples: list[Sample]) -> tuple[float, float, float]:
    if not samples:
        return (0.0, 0.0, 0.0)
    count = len(samples)
    return (
        sum(sample.x for sample in samples) / count,
        sum(sample.y for sample in samples) / count,
        sum(sample.z for sample in samples) / count,
    )


def response_profile(
    samples: list[Sample],
    center: tuple[float, float, float],
    baseline_rms: float,
    lead_count: int,
    sweep_count: int,
    start_hz: float,
    end_hz: float,
    mg_per_count: float,
) -> list[tuple[int, int, float]]:
    edges = tuple(
        round(start_hz + (end_hz - start_hz) * index / 8)
        for index in range(9)
    )
    profile: list[tuple[int, int, float]] = []
    for low, high in zip(edges, edges[1:]):
        begin = lead_count + round(
            (low - start_hz) / (end_hz - start_hz) * sweep_count
        )
        finish = lead_count + round(
            (high - start_hz) / (end_hz - start_hz) * sweep_count
        )
        measured = vector_rms(samples[max(0, begin) : finish], center) * mg_per_count
        response = math.sqrt(max(0.0, measured * measured - baseline_rms**2))
        profile.append((low, high, response))
    return profile


def print_summary(lines: list[str], path: Path) -> None:
    begin = parse_fields(lines[0])
    end = parse_fields(lines[-1])
    samples = parse_samples(lines)
    rate = int(begin.get("imu_rate_hz", "1600"))
    lead_count = rate * int(begin.get("lead_ms", "50")) // 1000
    sweep_count = rate * int(begin.get("sweep_ms", "250")) // 1000
    tail_count = rate * int(begin.get("tail_ms", "200")) // 1000
    full_scale_g = float(begin.get("range_g", "2"))
    mg_per_count = full_scale_g * 1000.0 / 32768.0

    reported = int(end.get("samples", "-1"))
    expected = int(end.get("expected", "-1"))
    if reported != len(samples):
        raise ValueError(
            f"END reports {reported} samples, but CSV contains {len(samples)}"
        )
    if end.get("valid") != "1":
        raise ValueError(f"firmware marked capture invalid: {lines[-1]}")
    tolerance = expected // 50
    if expected < 1 or abs(len(samples) - expected) > tolerance:
        raise ValueError(
            f"sample count {len(samples)} is outside {expected} +/- {tolerance}"
        )
    required = lead_count + sweep_count + tail_count
    if len(samples) < required:
        raise ValueError(
            f"capture has {len(samples)} samples; analysis windows need {required}"
        )

    baseline = samples[:lead_count]
    sweep = samples[lead_count : lead_count + sweep_count]
    decay = samples[
        lead_count + sweep_count : lead_count + sweep_count + tail_count
    ]
    center = mean_axes(baseline)
    baseline_rms = vector_rms(baseline, center) * mg_per_count
    sweep_rms = vector_rms(sweep, center) * mg_per_count
    decay_rms = vector_rms(decay, center) * mg_per_count
    if baseline_rms > 0.0 and math.isfinite(sweep_rms):
        snr_db = 20.0 * math.log10(sweep_rms / baseline_rms)
        snr_text = f"{snr_db:+.1f} dB over baseline"
    else:
        snr_text = "unavailable"

    print(f"saved: {path}")
    print(f"samples: {len(samples)} / expected {expected}")
    print(f"baseline AC RMS: {baseline_rms:.2f} mg")
    print(f"sweep-window RMS: {sweep_rms:.2f} mg ({snr_text})")
    print(f"decay-window RMS: {decay_rms:.2f} mg")
    if begin.get("stimulus") == "chirp":
        profile = response_profile(
            samples,
            center,
            baseline_rms,
            lead_count,
            sweep_count,
            float(begin.get("f0_hz", "180")),
            float(begin.get("f1_hz", "650")),
            mg_per_count,
        )
        formatted = "  ".join(
            f"{low}-{high}:{response:.1f}" for low, high, response in profile
        )
        print(f"chirp response (mg RMS): {formatted}")
    print(
        "transport: "
        f"valid={end.get('valid', '?')} "
        f"i2c_errors={end.get('i2c_errors', '?')} "
        f"fifo_peak={end.get('fifo_peak_bytes', '?')} B "
        f"play_ok={end.get('play_ok', '?')}"
    )


def safe_label(label: str) -> str:
    cleaned = re.sub(r"[^a-zA-Z0-9_-]+", "_", label).strip("_")
    return cleaned or "unlabelled"


def capture_one(
    port: serial.Serial, mode: str, label: str, output_dir: Path, timeout: float
) -> Path:
    command = b"g" if mode == "chirp" else b"n"
    lines = read_capture(port, command, timeout)
    stamp = datetime.now().astimezone().strftime("%Y%m%d_%H%M%S_%f")
    output_dir.mkdir(parents=True, exist_ok=True)
    path = output_dir / f"body_probe_{safe_label(label)}_{mode}_{stamp}.csv"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print_summary(lines, path)
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial port; auto-detected by VID/PID")
    parser.add_argument(
        "--mode", choices=("chirp", "noise", "both"), default="chirp"
    )
    parser.add_argument("--label", default="surface", help="surface/run label")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).parent / "out" / "body_probe",
    )
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--repeat", type=int, default=1)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be at least 1")

    device = args.port or discover_port()
    modes = ("noise", "chirp") if args.mode == "both" else (args.mode,)
    print(f"opening {device}")
    with serial.Serial(device, 115200, timeout=0.2, write_timeout=1.0) as port:
        time.sleep(1.0)
        port.write(b"?\n")
        port.flush()
        time.sleep(0.1)
        capture_index = 0
        for repeat in range(args.repeat):
            for mode in modes:
                if capture_index:
                    print()
                    time.sleep(0.2)
                if args.repeat > 1:
                    print(f"repeat {repeat + 1}/{args.repeat}, {mode}")
                capture_one(port, mode, args.label, args.output_dir, args.timeout)
                capture_index += 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc
