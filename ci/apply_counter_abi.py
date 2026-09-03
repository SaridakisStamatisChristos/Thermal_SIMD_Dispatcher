#!/usr/bin/env python3
from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))

replace_once(
    "CMakeLists.txt",
    "    src/runtime_guard.c\n    src/logging.c\n",
    "    src/runtime_guard.c\n    src/workload_counter.c\n    src/logging.c\n",
    "cmake workload counter",
)
replace_once(
    "Makefile",
    "\tsrc/runtime_guard.c \\\n\tsrc/runtime_api.c \\\n",
    "\tsrc/runtime_guard.c \\\n\tsrc/workload_counter.c \\\n\tsrc/runtime_api.c \\\n",
    "make workload counter",
)
replace_once(
    "src/thermal_perf.c",
    "_Atomic(uint64_t) g_tsd_workload_iterations = 0;\n\n",
    "",
    "remove duplicate workload counter storage",
)

# C++ translation units must not name the raw C11 workload atomic.
for root in (Path("src"), Path("tests")):
    for path in root.rglob("*.cpp"):
        if "g_tsd_workload_iterations" in path.read_text():
            raise SystemExit(f"raw workload atomic referenced from C++: {path}")

print("workload counter ABI migration applied")
