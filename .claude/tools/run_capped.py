#!/usr/bin/env python3
"""Run the editor/runtime under a memory ceiling, on every host the agents use.

Usage: python .claude/tools/run_capped.py <command...>      Ceiling: DESERT_MEM_CAP_MB (default 6144 on macOS,
16384 on Windows).

macOS: delegates to ~/.claude/tools/run_capped.sh (phys_footprint polling; unified memory makes GPU allocations
count, see that script). 2026-09-24 one editor grew to 13.7 GB of 16 and the kernel watchdog panicked the machine.

Windows (added 2026-09-27 for the render-graph programme, which runs there): the process is put into a Job Object
with JOB_OBJECT_LIMIT_PROCESS_MEMORY, so the OS itself refuses commits above the ceiling (children inherit the job).
The guard (agent_guard.py) refused every editor run on Windows because only the macOS wrapper existed.
"""
import os
import subprocess
import sys


def run_windows(argv):
    import ctypes
    from ctypes import wintypes

    cap_mb = int(os.environ.get("DESERT_MEM_CAP_MB", "16384"))
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in (
            "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
            "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class BASIC_LIMIT(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class EXTENDED_LIMIT(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BASIC_LIMIT), ("IoInfo", IO_COUNTERS),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100
    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
    EXTENDED_LIMIT_INFORMATION = 9

    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        print(f"[run_capped] CreateJobObject failed ({ctypes.get_last_error()})", file=sys.stderr)
        return 2
    info = EXTENDED_LIMIT()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    info.ProcessMemoryLimit = cap_mb * 1024 * 1024
    if not kernel32.SetInformationJobObject(job, EXTENDED_LIMIT_INFORMATION, ctypes.byref(info),
                                            ctypes.sizeof(info)):
        print(f"[run_capped] SetInformationJobObject failed ({ctypes.get_last_error()})", file=sys.stderr)
        return 2

    process = subprocess.Popen(argv)
    handle = kernel32.OpenProcess(0x0100 | 0x0001, False, process.pid)  # SET_QUOTA | TERMINATE
    if not handle or not kernel32.AssignProcessToJobObject(job, handle):
        process.kill()
        print(f"[run_capped] AssignProcessToJobObject failed ({ctypes.get_last_error()})", file=sys.stderr)
        return 2
    rc = process.wait()
    kernel32.QueryInformationJobObject(job, EXTENDED_LIMIT_INFORMATION, ctypes.byref(info), ctypes.sizeof(info),
                                       None)
    peak_mb = info.PeakProcessMemoryUsed // (1024 * 1024)
    if peak_mb >= cap_mb:
        print(f"[run_capped] memory reached the cap {cap_mb} MB: this is a defect to report (which scene, which "
              "step), not a limit to raise.", file=sys.stderr)
    print(f"[run_capped] exit={rc} peak={peak_mb} MB", file=sys.stderr)
    return rc


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    if sys.platform == "win32":
        return run_windows(argv)
    script = os.path.expanduser("~/.claude/tools/run_capped.sh")
    return subprocess.call([script, *argv])


if __name__ == "__main__":
    sys.exit(main())
