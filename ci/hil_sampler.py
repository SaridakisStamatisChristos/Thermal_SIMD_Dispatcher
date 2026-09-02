#!/usr/bin/env python3
"""Sample a live thermal-simd runtime into reproducible HIL evidence.

The sampler deliberately depends only on the Python standard library and Linux
sysfs. It records the runtime's own health snapshot together with CPU frequency
and package-energy deltas so a HIL run can be audited after the workflow ends.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import glob
import json
import math
import pathlib
import time
import urllib.error
import urllib.request
from collections import Counter
from typing import Any


def read_int(path: pathlib.Path) -> int | None:
    try:
        return int(path.read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        return None


def discover_rapl_domains() -> list[tuple[pathlib.Path, int | None]]:
    domains: list[tuple[pathlib.Path, int | None]] = []
    patterns = (
        "/sys/class/powercap/intel-rapl:[0-9]*/energy_uj",
        "/sys/class/powercap/amd-rapl:[0-9]*/energy_uj",
    )
    for pattern in patterns:
        for raw in sorted(glob.glob(pattern)):
            energy = pathlib.Path(raw)
            max_range = read_int(energy.with_name("max_energy_range_uj"))
            domains.append((energy, max_range))
    return domains


def energy_sum_uj(domains: list[tuple[pathlib.Path, int | None]]) -> tuple[int | None, list[int | None]]:
    if not domains:
        return None, []
    values = [read_int(path) for path, _ in domains]
    if any(value is None for value in values):
        return None, values
    return sum(int(value) for value in values if value is not None), values


def energy_delta_uj(
    previous: list[int | None],
    current: list[int | None],
    domains: list[tuple[pathlib.Path, int | None]],
) -> int | None:
    if not domains or len(previous) != len(current) or len(current) != len(domains):
        return None
    total = 0
    for old, new, (_, max_range) in zip(previous, current, domains):
        if old is None or new is None:
            return None
        if new >= old:
            total += new - old
        elif max_range is not None and max_range > old:
            total += (max_range - old) + new
        else:
            return None
    return total


def fetch_json(url: str, timeout: float) -> dict[str, Any] | None:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            if response.status != 200:
                return None
            return json.loads(response.read().decode("utf-8"))
    except (OSError, urllib.error.URLError, json.JSONDecodeError):
        return None


def finite_number(value: Any) -> float | None:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def read_cpu_frequency_khz(cpu: int | None) -> int | None:
    if cpu is None or cpu < 0:
        return None
    base = pathlib.Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
    for name in ("scaling_cur_freq", "cpuinfo_cur_freq"):
        value = read_int(base / name)
        if value is not None and value > 0:
            return value
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True, help="thermal-simd /healthz URL")
    parser.add_argument("--duration-seconds", type=float, required=True)
    parser.add_argument("--interval-seconds", type=float, default=1.0)
    parser.add_argument("--csv", required=True)
    parser.add_argument("--jsonl", required=True)
    parser.add_argument("--summary", required=True)
    args = parser.parse_args()

    if args.duration_seconds <= 0 or args.interval_seconds <= 0:
        parser.error("duration and interval must be positive")

    csv_path = pathlib.Path(args.csv)
    jsonl_path = pathlib.Path(args.jsonl)
    summary_path = pathlib.Path(args.summary)
    for path in (csv_path, jsonl_path, summary_path):
        path.parent.mkdir(parents=True, exist_ok=True)

    rapl_domains = discover_rapl_domains()
    _, previous_energy_values = energy_sum_uj(rapl_domains)
    previous_energy_time = time.monotonic()

    fieldnames = [
        "utc",
        "elapsed_s",
        "live",
        "ready",
        "current_width",
        "recommended_width",
        "perf_mode",
        "counters_healthy",
        "pinned_cpu",
        "monitor_cpu",
        "fusion_degraded",
        "temp_available",
        "package_temp_c",
        "freq_available",
        "freq_ratio",
        "cpu_cur_khz",
        "rapl_power_w",
    ]

    started = time.monotonic()
    deadline = started + args.duration_seconds
    sample_count = 0
    health_samples = 0
    live_samples = 0
    ready_samples = 0
    hardware_perf_samples = 0
    temp_samples = 0
    temperatures: list[float] = []
    powers: list[float] = []
    widths: Counter[str] = Counter()

    with csv_path.open("w", newline="", encoding="utf-8") as csv_file, jsonl_path.open(
        "w", encoding="utf-8"
    ) as jsonl_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()

        next_sample = started
        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            if now < next_sample:
                time.sleep(min(next_sample - now, 0.1))
                continue

            health = fetch_json(args.url, timeout=min(1.0, args.interval_seconds))
            timestamp = dt.datetime.now(dt.timezone.utc).isoformat()
            elapsed = now - started

            rapl_power: float | None = None
            _, current_energy_values = energy_sum_uj(rapl_domains)
            energy_now = time.monotonic()
            delta_energy = energy_delta_uj(previous_energy_values, current_energy_values, rapl_domains)
            delta_time = energy_now - previous_energy_time
            if delta_energy is not None and delta_time > 0:
                rapl_power = (delta_energy / 1_000_000.0) / delta_time
                if math.isfinite(rapl_power) and rapl_power >= 0:
                    powers.append(rapl_power)
                else:
                    rapl_power = None
            previous_energy_values = current_energy_values
            previous_energy_time = energy_now

            row: dict[str, Any] = {
                "utc": timestamp,
                "elapsed_s": f"{elapsed:.3f}",
                "live": "",
                "ready": "",
                "current_width": "",
                "recommended_width": "",
                "perf_mode": "",
                "counters_healthy": "",
                "pinned_cpu": "",
                "monitor_cpu": "",
                "fusion_degraded": "",
                "temp_available": "",
                "package_temp_c": "",
                "freq_available": "",
                "freq_ratio": "",
                "cpu_cur_khz": "",
                "rapl_power_w": "" if rapl_power is None else f"{rapl_power:.6f}",
            }

            if health is not None:
                health_samples += 1
                jsonl_file.write(json.dumps({"utc": timestamp, "health": health}, sort_keys=True) + "\n")
                controller = health.get("controller", {})
                fusion = health.get("fusion", {})
                perf = health.get("perf", {})
                live = bool(health.get("live", False))
                ready = bool(health.get("ready", False))
                if live:
                    live_samples += 1
                if ready:
                    ready_samples += 1
                mode = str(perf.get("mode", ""))
                counters_healthy = bool(perf.get("countersHealthy", False))
                if mode == "hardware" and counters_healthy:
                    hardware_perf_samples += 1
                width = str(controller.get("currentWidth", ""))
                if width:
                    widths[width] += 1
                temp_available = bool(fusion.get("tempAvailable", False))
                temp = finite_number(fusion.get("packageTempC")) if temp_available else None
                if temp is not None:
                    temp_samples += 1
                    temperatures.append(temp)
                pinned_raw = perf.get("pinnedCpu")
                try:
                    pinned_cpu = int(pinned_raw)
                except (TypeError, ValueError):
                    pinned_cpu = None
                cur_khz = read_cpu_frequency_khz(pinned_cpu)

                row.update(
                    {
                        "live": int(live),
                        "ready": int(ready),
                        "current_width": width,
                        "recommended_width": controller.get("recommendedWidth", ""),
                        "perf_mode": mode,
                        "counters_healthy": int(counters_healthy),
                        "pinned_cpu": "" if pinned_cpu is None else pinned_cpu,
                        "monitor_cpu": perf.get("monitorCpu", ""),
                        "fusion_degraded": int(bool(fusion.get("degraded", False))),
                        "temp_available": int(temp_available),
                        "package_temp_c": "" if temp is None else f"{temp:.3f}",
                        "freq_available": int(bool(fusion.get("freqAvailable", False))),
                        "freq_ratio": fusion.get("freqRatio", ""),
                        "cpu_cur_khz": "" if cur_khz is None else cur_khz,
                    }
                )
            else:
                jsonl_file.write(json.dumps({"utc": timestamp, "health": None}, sort_keys=True) + "\n")

            writer.writerow(row)
            csv_file.flush()
            jsonl_file.flush()
            sample_count += 1
            next_sample += args.interval_seconds
            if next_sample < time.monotonic() - args.interval_seconds:
                next_sample = time.monotonic()

    denominator = max(sample_count, 1)
    summary = {
        "duration_seconds": time.monotonic() - started,
        "sample_count": sample_count,
        "health_sample_fraction": health_samples / denominator,
        "live_fraction": live_samples / denominator,
        "ready_fraction": ready_samples / denominator,
        "hardware_perf_fraction": hardware_perf_samples / denominator,
        "temperature_sample_fraction": temp_samples / denominator,
        "max_temperature_c": max(temperatures) if temperatures else None,
        "mean_temperature_c": sum(temperatures) / len(temperatures) if temperatures else None,
        "mean_rapl_power_w": sum(powers) / len(powers) if powers else None,
        "max_rapl_power_w": max(powers) if powers else None,
        "rapl_domain_count": len(rapl_domains),
        "width_samples": dict(sorted(widths.items())),
    }
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
