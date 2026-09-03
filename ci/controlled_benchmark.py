#!/usr/bin/env python3
"""Run balanced fixed-width registered-kernel controls on bare metal.

The executable performs identical integer work in each SIMD implementation.
This driver alternates mode order, runs independent warmups, verifies checksums,
and samples package energy/temperature without requiring third-party modules.
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import math
import os
import pathlib
import statistics
import subprocess
import sys
import time
from typing import Any


def read_int(path: pathlib.Path) -> int | None:
    try:
        return int(path.read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        return None


def read_text(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def discover_rapl_domains() -> list[tuple[pathlib.Path, int | None]]:
    domains: list[tuple[pathlib.Path, int | None]] = []
    for pattern in (
        "/sys/class/powercap/intel-rapl:[0-9]*/energy_uj",
        "/sys/class/powercap/amd-rapl:[0-9]*/energy_uj",
    ):
        for raw_path in sorted(glob.glob(pattern)):
            energy_path = pathlib.Path(raw_path)
            # Class entries for subdomains (for example core and DRAM) are
            # siblings of the package entry on many kernels.  Counting both
            # would double-count energy already included by the package.
            if energy_path.parent.name.count(":") != 1:
                continue
            domains.append((energy_path, read_int(energy_path.with_name("max_energy_range_uj"))))
    return domains


def energy_values(domains: list[tuple[pathlib.Path, int | None]]) -> list[int] | None:
    values = [read_int(path) for path, _ in domains]
    if not domains or any(value is None for value in values):
        return None
    return [int(value) for value in values if value is not None]


def energy_delta_uj(
    previous: list[int],
    current: list[int],
    domains: list[tuple[pathlib.Path, int | None]],
) -> int | None:
    if len(previous) != len(current) or len(current) != len(domains):
        return None
    total = 0
    for old, new, (_, maximum) in zip(previous, current, domains):
        if new >= old:
            total += new - old
        elif maximum is not None and maximum > old:
            total += (maximum - old) + new
        else:
            return None
    return total


class EnergyAccumulator:
    def __init__(self, domains: list[tuple[pathlib.Path, int | None]]) -> None:
        self.domains = domains
        self.previous = energy_values(domains)
        self.total_uj = 0
        self.valid = self.previous is not None

    def sample(self) -> None:
        if not self.valid or self.previous is None:
            return
        current = energy_values(self.domains)
        if current is None:
            self.valid = False
            return
        delta = energy_delta_uj(self.previous, current, self.domains)
        if delta is None:
            self.valid = False
            return
        self.total_uj += delta
        self.previous = current

    def joules(self) -> float | None:
        return self.total_uj / 1_000_000.0 if self.valid else None


def discover_package_temperatures() -> list[pathlib.Path]:
    preferred: list[pathlib.Path] = []
    fallback: list[pathlib.Path] = []
    cpu_sensor_names = {"coretemp", "k10temp", "zenpower", "zenpower3", "amd_energy"}
    for hwmon_raw in sorted(glob.glob("/sys/class/hwmon/hwmon*")):
        hwmon = pathlib.Path(hwmon_raw)
        sensor_name = read_text(hwmon / "name").lower()
        if sensor_name not in cpu_sensor_names:
            continue
        for input_raw in sorted(glob.glob(str(hwmon / "temp*_input"))):
            input_path = pathlib.Path(input_raw)
            label = read_text(input_path.with_name(input_path.name.replace("_input", "_label"))).lower()
            if any(token in label for token in ("package", "tctl", "tdie")):
                preferred.append(input_path)
            else:
                fallback.append(input_path)
    if preferred:
        return preferred

    thermal_fallback: list[pathlib.Path] = []
    for temp_raw in sorted(glob.glob("/sys/class/thermal/thermal_zone*/temp")):
        temp_path = pathlib.Path(temp_raw)
        sensor_type = read_text(temp_path.with_name("type")).lower()
        if any(token in sensor_type for token in ("x86_pkg_temp", "cpu", "soc", "package")):
            thermal_fallback.append(temp_path)
    return fallback or thermal_fallback


def read_package_temperature_c(paths: list[pathlib.Path]) -> float | None:
    values: list[float] = []
    for path in paths:
        raw = read_int(path)
        if raw is None:
            continue
        value = raw / 1000.0
        if math.isfinite(value) and -50.0 <= value <= 150.0:
            values.append(value)
    return max(values) if values else None


def allowed_cpus() -> set[int] | None:
    try:
        allowed = os.sched_getaffinity(0)
    except (AttributeError, OSError):
        return None
    return set(allowed) if allowed else None


def read_cpu_frequency_khz(cpu: int | None) -> int | None:
    if cpu is None:
        return None
    base = pathlib.Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
    for filename in ("scaling_cur_freq", "cpuinfo_cur_freq"):
        value = read_int(base / filename)
        if value is not None and value > 0:
            return value
    return None


def benchmark_command(
    executable: pathlib.Path,
    mode: str,
    duration_seconds: float,
    work_items: int,
    chunk_items: int,
    rounds: int,
    result_path: pathlib.Path | None,
) -> list[str]:
    command = [
        str(executable),
        f"--mode={mode}",
        f"--duration-seconds={duration_seconds}",
        f"--work-items={work_items}",
        f"--chunk-items={chunk_items}",
        f"--rounds={rounds}",
    ]
    if result_path is not None:
        command.append(f"--result-json={result_path}")
    return command


def start_benchmark_process(
    command: list[str],
    log_file: Any,
    launch_affinity: set[int] | None,
    sampler_cpu: int | None,
) -> subprocess.Popen[Any]:
    if launch_affinity is not None:
        os.sched_setaffinity(0, launch_affinity)
    process: subprocess.Popen[Any] | None = None
    try:
        process = subprocess.Popen(command, stdout=log_file, stderr=subprocess.STDOUT)
        # The child inherits the complete allowed mask and pins itself to its
        # first CPU. Keep this sampling process off that CPU where possible.
        if sampler_cpu is not None:
            os.sched_setaffinity(0, {sampler_cpu})
        return process
    except BaseException:
        if process is not None:
            terminate_process(process)
        raise


def run_warmup(
    args: argparse.Namespace,
    mode: str,
    log_path: pathlib.Path,
    launch_affinity: set[int] | None,
    sampler_cpu: int | None,
) -> None:
    if args.warmup_seconds <= 0:
        return
    command = benchmark_command(
        args.executable,
        mode,
        args.warmup_seconds,
        args.work_items,
        args.chunk_items,
        args.rounds,
        None,
    )
    with log_path.open("a", encoding="utf-8") as log_file:
        log_file.write(f"warmup_command={command!r}\n")
        log_file.flush()
        process = start_benchmark_process(command, log_file, launch_affinity, sampler_cpu)
        try:
            return_code = process.wait(timeout=args.warmup_seconds + 30.0)
        except BaseException:
            terminate_process(process)
            raise
    if return_code != 0:
        raise RuntimeError(f"{mode} warmup failed with status {return_code}; see {log_path}")


def terminate_process(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)


def run_trial(
    args: argparse.Namespace,
    mode: str,
    trial_index: int,
    sequence_index: int,
    rapl_domains: list[tuple[pathlib.Path, int | None]],
    temperature_paths: list[pathlib.Path],
    workload_cpu: int | None,
    sampler_cpu: int | None,
    launch_affinity: set[int] | None,
) -> dict[str, Any]:
    stem = f"trial-{trial_index + 1:02d}-{sequence_index + 1:02d}-{mode}"
    result_path = args.log_dir / f"{stem}.json"
    log_path = args.log_dir / f"{stem}.log"
    run_warmup(args, mode, log_path, launch_affinity, sampler_cpu)

    command = benchmark_command(
        args.executable,
        mode,
        args.duration_seconds,
        args.work_items,
        args.chunk_items,
        args.rounds,
        result_path,
    )
    energy = EnergyAccumulator(rapl_domains)
    temperatures: list[float] = []
    frequencies: list[int] = []
    started = time.monotonic()
    with log_path.open("a", encoding="utf-8") as log_file:
        log_file.write(f"measurement_command={command!r}\n")
        log_file.flush()
        process = start_benchmark_process(command, log_file, launch_affinity, sampler_cpu)
        deadline = started + args.duration_seconds + 30.0
        try:
            while True:
                energy.sample()
                temperature = read_package_temperature_c(temperature_paths)
                if temperature is not None:
                    temperatures.append(temperature)
                frequency = read_cpu_frequency_khz(workload_cpu)
                if frequency is not None:
                    frequencies.append(frequency)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    terminate_process(process)
                    raise RuntimeError(f"{mode} trial exceeded its timeout; see {log_path}")
                try:
                    return_code = process.wait(timeout=min(args.sample_interval_seconds, remaining))
                    break
                except subprocess.TimeoutExpired:
                    continue
        except BaseException:
            terminate_process(process)
            raise
        energy.sample()
    external_elapsed = time.monotonic() - started

    if return_code != 0:
        raise RuntimeError(f"{mode} trial failed with status {return_code}; see {log_path}")
    try:
        result = json.loads(result_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"invalid result for {mode} trial {trial_index + 1}: {exc}") from exc
    if result.get("mode") != mode or result.get("completed_passes", 0) < 1:
        raise RuntimeError(f"incomplete result for {mode} trial {trial_index + 1}")
    if workload_cpu is not None and result.get("pinned_cpu") != workload_cpu:
        raise RuntimeError(
            f"{mode} trial pinned CPU {result.get('pinned_cpu')} instead of {workload_cpu}"
        )

    energy_j = energy.joules()
    completed_items = int(result["completed_work_items"])
    result.update(
        {
            "trial": trial_index + 1,
            "sequence": sequence_index + 1,
            "external_elapsed_seconds": external_elapsed,
            "package_energy_j": energy_j,
            "average_package_power_w": (
                energy_j / external_elapsed if energy_j is not None and external_elapsed > 0 else None
            ),
            "items_per_joule": (
                completed_items / energy_j if energy_j is not None and energy_j > 0 else None
            ),
            "mean_package_temperature_c": (
                statistics.fmean(temperatures) if temperatures else None
            ),
            "max_package_temperature_c": max(temperatures) if temperatures else None,
            "mean_cpu_frequency_khz": statistics.fmean(frequencies) if frequencies else None,
            "log_file": log_path.name,
        }
    )
    return result


def median_or_none(values: list[float | None]) -> float | None:
    present = [float(value) for value in values if value is not None and math.isfinite(float(value))]
    return statistics.median(present) if present else None


def median_absolute_deviation(values: list[float]) -> float:
    center = statistics.median(values)
    return statistics.median(abs(value - center) for value in values)


def aggregate_mode(trials: list[dict[str, Any]]) -> dict[str, Any]:
    throughputs = [float(trial["items_per_second"]) for trial in trials]
    return {
        "trial_count": len(trials),
        "median_items_per_second": statistics.median(throughputs),
        "throughput_mad_items_per_second": median_absolute_deviation(throughputs),
        "min_items_per_second": min(throughputs),
        "max_items_per_second": max(throughputs),
        "median_package_power_w": median_or_none(
            [trial.get("average_package_power_w") for trial in trials]
        ),
        "median_items_per_joule": median_or_none([trial.get("items_per_joule") for trial in trials]),
        "max_package_temperature_c": max(
            (
                float(trial["max_package_temperature_c"])
                for trial in trials
                if trial.get("max_package_temperature_c") is not None
            ),
            default=None,
        ),
    }


def write_csv(path: pathlib.Path, trials: list[dict[str, Any]]) -> None:
    fields = [
        "trial",
        "sequence",
        "mode",
        "elapsed_seconds",
        "external_elapsed_seconds",
        "completed_passes",
        "completed_work_items",
        "items_per_second",
        "checksum_fnv1a64",
        "package_energy_j",
        "average_package_power_w",
        "items_per_joule",
        "mean_package_temperature_c",
        "max_package_temperature_c",
        "mean_cpu_frequency_khz",
        "pinned_cpu",
        "log_file",
    ]
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(trials)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=pathlib.Path)
    parser.add_argument("--target-isa", required=True, choices=("sse41", "avx2", "avx512"))
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--duration-seconds", type=float, default=5.0)
    parser.add_argument("--warmup-seconds", type=float, default=1.0)
    parser.add_argument("--cooldown-seconds", type=float, default=2.0)
    parser.add_argument("--sample-interval-seconds", type=float, default=0.1)
    parser.add_argument("--work-items", type=int, default=1048576)
    parser.add_argument("--chunk-items", type=int, default=65536)
    parser.add_argument("--rounds", type=int, default=32)
    parser.add_argument("--output-json", required=True, type=pathlib.Path)
    parser.add_argument("--output-csv", required=True, type=pathlib.Path)
    parser.add_argument("--log-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()

    if not args.executable.is_file() or not os.access(args.executable, os.X_OK):
        parser.error("--executable must name an executable file")
    if args.trials < 2 or args.trials > 50:
        parser.error("--trials must be in [2, 50]")
    if not 0.05 <= args.duration_seconds <= 3600.0:
        parser.error("--duration-seconds must be in [0.05, 3600]")
    if not 0.0 <= args.warmup_seconds <= 300.0:
        parser.error("--warmup-seconds must be in [0, 300]")
    if not 0.0 <= args.cooldown_seconds <= 600.0:
        parser.error("--cooldown-seconds must be in [0, 600]")
    if not 0.05 <= args.sample_interval_seconds <= 1.0:
        parser.error("--sample-interval-seconds must be in [0.05, 1]")
    if (
        args.work_items < 16
        or args.chunk_items < 16
        or args.chunk_items > args.work_items
        or args.work_items % 16 != 0
        or args.chunk_items % 16 != 0
    ):
        parser.error("work and chunk item counts must be multiples of 16, with chunk <= work")
    if args.rounds < 1 or args.rounds > 10000:
        parser.error("--rounds must be in [1, 10000]")

    for path in (args.output_json, args.output_csv):
        path.parent.mkdir(parents=True, exist_ok=True)
    args.log_dir.mkdir(parents=True, exist_ok=True)

    modes = ["sse41"]
    if args.target_isa in ("avx2", "avx512"):
        modes.append("avx2")
    if args.target_isa == "avx512":
        modes.append("avx512")
    rapl_domains = discover_rapl_domains()
    temperature_paths = discover_package_temperatures()
    launch_affinity = allowed_cpus()
    ordered_cpus = sorted(launch_affinity) if launch_affinity is not None else []
    workload_cpu = ordered_cpus[0] if ordered_cpus else None
    sampler_cpu = ordered_cpus[1] if len(ordered_cpus) > 1 else workload_cpu

    trials: list[dict[str, Any]] = []
    sequence_index = 0
    expected_checksum: str | None = None
    for trial_index in range(args.trials):
        ordered_modes = modes if trial_index % 2 == 0 else list(reversed(modes))
        for mode in ordered_modes:
            if sequence_index > 0 and args.cooldown_seconds > 0:
                time.sleep(args.cooldown_seconds)
            result = run_trial(
                args,
                mode,
                trial_index,
                sequence_index,
                rapl_domains,
                temperature_paths,
                workload_cpu,
                sampler_cpu,
                launch_affinity,
            )
            checksum = str(result["checksum_fnv1a64"])
            if expected_checksum is None:
                expected_checksum = checksum
            elif checksum != expected_checksum:
                raise RuntimeError(
                    f"checksum mismatch: expected {expected_checksum}, got {checksum} from {mode}"
                )
            trials.append(result)
            sequence_index += 1

    aggregates = {
        mode: aggregate_mode([trial for trial in trials if trial["mode"] == mode]) for mode in modes
    }
    baseline = float(aggregates["sse41"]["median_items_per_second"])
    comparisons: dict[str, Any] = {}
    for mode in modes[1:]:
        mode_throughput = float(aggregates[mode]["median_items_per_second"])
        comparisons[f"{mode}_vs_sse41"] = {
            "median_throughput_ratio": mode_throughput / baseline if baseline > 0 else None,
        }
        baseline_efficiency = aggregates["sse41"]["median_items_per_joule"]
        mode_efficiency = aggregates[mode]["median_items_per_joule"]
        comparisons[f"{mode}_vs_sse41"]["median_efficiency_ratio"] = (
            float(mode_efficiency) / float(baseline_efficiency)
            if mode_efficiency is not None and baseline_efficiency not in (None, 0)
            else None
        )

    summary = {
        "schema_version": 1,
        "target_isa": args.target_isa,
        "settings": {
            "trials_per_mode": args.trials,
            "duration_seconds": args.duration_seconds,
            "warmup_seconds": args.warmup_seconds,
            "cooldown_seconds": args.cooldown_seconds,
            "sample_interval_seconds": args.sample_interval_seconds,
            "work_items": args.work_items,
            "chunk_items": args.chunk_items,
            "rounds": args.rounds,
        },
        "rapl_domain_count": len(rapl_domains),
        "rapl_domains": [
            {"energy_path": str(path), "max_energy_range_uj": maximum}
            for path, maximum in rapl_domains
        ],
        "temperature_sources": [str(path) for path in temperature_paths],
        "affinity": {
            "allowed_cpus": ordered_cpus,
            "workload_cpu": workload_cpu,
            "sampler_cpu": sampler_cpu,
            "separated": (
                workload_cpu is not None and sampler_cpu is not None and workload_cpu != sampler_cpu
            ),
        },
        "expected_checksum_fnv1a64": expected_checksum,
        "modes": aggregates,
        "comparisons": comparisons,
        "trials": trials,
    }
    args.output_json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_csv(args.output_csv, trials)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"controlled benchmark failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
