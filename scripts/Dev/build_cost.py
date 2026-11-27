#!/usr/bin/env python3
"""Where the compile time goes (STUDIO-AUDIT B-GATES): the slowest objects, and with DESERT_TIME_TRACE=ON why.

    python scripts/Dev/build_cost.py --build <dir> [--log <build output>] [--top N]

--build is a configured ninja tree (CMakeCache.txt, .ninja_log). Always printed: the N slowest objects of the last
ninja session in .ninja_log (seconds, object). With DESERT_TIME_TRACE=ON the compiler's own account follows:
  clang  -ftime-trace leaves <object>.json beside every object; ClangBuildAnalyzer (on PATH) sums them into the
         time summary and the N most expensive headers by full path (parse seconds, include count).
  MSVC   /Bt+ prints "time(...c1xx.dll)=<s>" (front end) and "time(...c2.dll)=<s>" (back end) per source and
         /d2cgsummary lists the functions whose code generation was anomalously slow; both go to the compiler's
         output, which ninja prints, so --log names the captured build output to parse.
The last line is "TOTAL_TOP_SECONDS <n>" (the sum of the listed objects), for a before/after diff to grep.
Exit 1 = a usage or input error.
"""
import os
import re
import shutil
import subprocess
import sys

INI = "[counts]\nfileParse=10\nfileCodegen=10\nfunction=10\ntemplate=10\ntemplateSet=10\nheader={top}\nheaderChain=3\n"
BT = re.compile(r"time\(.*?(c1xx|c2)\.dll\)=([0-9.]+)s.*\[(.+?)\]\s*$")
CG_HEAD = re.compile(r"Anomalistic Compile Times", re.I)
CG_ROW = re.compile(r"^\s*(\S+): ([0-9.]+) sec")


def fail(message):
    print(f"build_cost: {message}", file=sys.stderr)
    raise SystemExit(1)


def arg(name, default=None):
    if name not in sys.argv:
        return default
    at = sys.argv.index(name) + 1
    if at >= len(sys.argv) or sys.argv[at].startswith("--"):
        fail(f"{name} needs a value")
    return sys.argv[at]


def cache_value(build, key):
    path = os.path.join(build, "CMakeCache.txt")
    if not os.path.isfile(path):
        fail(f"{build} is not a configured tree (no CMakeCache.txt)")
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith(key + ":"):
            return line.rstrip("\n").split("=", 1)[1]
    return ""


def slowest_objects(build, top):
    log = os.path.join(build, ".ninja_log")
    if not os.path.isfile(log):
        fail(f"no {log}: build the tree first")
    last = {}
    for line in open(log, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) == 5 and parts[0].isdigit() and parts[3].endswith((".obj", ".o")):
            last[parts[3]] = (int(parts[1]) - int(parts[0])) / 1000.0
    rows = sorted(last.items(), key=lambda kv: -kv[1])[:top]
    print(f"**** Top-{top} slowest objects (.ninja_log, seconds):")
    for out, seconds in rows:
        print(f"{seconds:9.1f} s  {out}")
    return sum(seconds for _, seconds in rows)


def clang_report(build, top):
    tool = shutil.which("ClangBuildAnalyzer")
    if not tool:
        fail("ClangBuildAnalyzer is not on PATH (brew install clangbuildanalyzer)")
    work = os.path.join(build, "BuildCost")
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, "ClangBuildAnalyzer.ini"), "w", encoding="utf-8") as ini:
        ini.write(INI.format(top=top))
    capture = os.path.join(work, "capture.bin")
    run = subprocess.run([tool, "--all", build, capture], capture_output=True, text=True)
    if run.returncode != 0:
        fail(f"ClangBuildAnalyzer --all failed: {run.stderr.strip()[:300]}")
    run = subprocess.run([tool, "--analyze", capture], capture_output=True, text=True, cwd=work)
    if run.returncode != 0:
        fail(f"ClangBuildAnalyzer --analyze failed: {run.stderr.strip()[:300]}")
    lines = run.stdout.splitlines()
    on = False
    for line in lines:
        if "Time summary" in line:
            on = True
        if on:
            print(line)
            if not line.strip():
                break
    print(f"**** Top-{top} expensive headers (parse seconds total, include count, full path):")
    on = False
    for line in lines:
        if re.search(r"\*\*\*.*Expensive headers", line):
            on = True
            continue
        if on and line.startswith("***"):
            break
        m = re.match(r"^(\d+) ms: (.+?) \(included (\d+) times", line) if on else None
        if m:
            print(f"{int(m.group(1)) / 1000.0:9.1f} s  {m.group(3):>5} x  {m.group(2)}")


def msvc_report(log, top):
    if not log or not os.path.isfile(log):
        fail("MSVC timings live in the build output: pass --log <the captured output of the build>")
    front, back, functions = {}, {}, []
    in_summary = False
    for line in open(log, encoding="utf-8", errors="replace"):
        m = BT.search(line)
        if m:
            table = front if m.group(1) == "c1xx" else back
            table[m.group(3)] = table.get(m.group(3), 0.0) + float(m.group(2))
            continue
        if CG_HEAD.search(line):
            in_summary = True
            continue
        row = CG_ROW.match(line) if in_summary else None
        if row:
            functions.append((float(row.group(2)), row.group(1)))
        elif in_summary and line.strip() and not line.startswith((" ", "\t")):
            in_summary = False
    if not front and not back:
        fail(f"{log} has no /Bt+ lines: configure with -DDESERT_TIME_TRACE=ON and rebuild")
    for title, table in (("front end (c1xx, parse + templates)", front), ("back end (c2, code generation)", back)):
        print(f"**** Top-{top} sources by {title}, seconds:")
        for source, seconds in sorted(table.items(), key=lambda kv: -kv[1])[:top]:
            print(f"{seconds:9.1f} s  {source}")
    print(f"**** Top-{top} functions by code generation time (/d2cgsummary), seconds:")
    for seconds, name in sorted(functions, reverse=True)[:top]:
        print(f"{seconds:9.2f} s  {name}")


def main():
    build = arg("--build")
    if not build:
        fail("usage: build_cost.py --build <dir> [--log <build output>] [--top N]")
    top_text = arg("--top", "30")
    if not top_text.isdigit() or int(top_text) == 0:
        fail("--top needs a positive number")
    top = int(top_text)
    total = slowest_objects(build, top)
    if cache_value(build, "DESERT_TIME_TRACE") == "ON":
        compiler = os.path.basename(cache_value(build, "CMAKE_CXX_COMPILER")).lower()
        if compiler in ("cl", "cl.exe"):
            msvc_report(arg("--log"), top)
        else:
            clang_report(build, top)
    print(f"TOTAL_TOP_SECONDS {total:.1f}")


if __name__ == "__main__":
    main()
