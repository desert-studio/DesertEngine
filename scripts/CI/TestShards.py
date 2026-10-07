#!/usr/bin/env python3
"""Split the test suites of one CI configuration into N shards, and prove afterwards that every
expected suite ran exactly once.

WHY SHARDS. CI run #900 (36426811427): Windows Debug spent 57 min building and then 29 min running
354 suites on the same 4-core runner; the ASan job spent 47 min building and ~41 min testing, and was
cancelled by its 90-minute ceiling. The build and the tests are now separate jobs: the build job uploads
the test binaries once, N shard jobs download them and each runs its own list. A failed shard is
re-run on its own and never rebuilds.

WHY A TIMING TABLE AND NOT FIFO. The distribution is a handful of heavy suites and three hundred light
ones (Windows Debug: ShaderCacheKey 577 s, CloudType 477 s, CloudField 401 s of 6008 suite-seconds;
ASan: JsonCensus 749 s of 6978). Split alphabetically, two heavy suites can land in one shard and that
shard sets the whole job's wall clock. Longest-processing-time-first over recorded times puts them
apart. The table (scripts/CI/TestTimings.tsv) can go stale; staleness costs BALANCE only, because the
plan is taken over the expected-suite list, never over the table.

Subcommands:
  plan    --expected build/TestManifest.txt   --timings TSV --column NAME --shards N --out DIR
          --vulkan-device-suites FILE --vulkan-runner-var NAME
          Writes DIR/shard-<i>.txt (i = 1..N, heaviest first so the long suites start first) and
          DIR/expected.txt. Fails unless the shards are disjoint and their union is the expected list.
          The suites in --vulkan-device-suites (build/TestNeedsVulkanDevice.txt, written by
          Desert/Tests/premake5.lua from its one `vulkan_device_suites` list) need a real Vulkan
          device. Unless the environment variable NAME is 'true' they are NOT expected on this runner:
          they are named in a ::notice and in DIR/needs-vulkan-device.txt, and verify fails if one of
          them reported anyway. With NAME 'true' they are planned like every other suite.
  verify  --plan DIR --reports DIR
          DIR holds one sub-directory per shard artifact (test-reports-...-shard-<i>/). Fails unless
          every expected suite left exactly one gtest XML, in the shard that was planned to run it.
  timings --reports DIR
          Prints "<suite>\t<ceil seconds>" from gtest XML reports, to refresh a column of the table.
"""

import argparse
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

COLUMNS = ("windows-debug", "windows-release", "asan")


def read_list(path):
    with open(path, encoding="utf-8") as f:
        return [line.strip() for line in f if line.strip()]


def read_manifest(path):
    # build/TestManifest.txt: `<Executable> <Suite>` per line (Desert/Tests/premake5.lua). Shards name suites.
    suites = []
    for line in read_list(path):
        fields = line.split()
        if len(fields) != 2:
            sys.exit(f"[ERROR] {path}: malformed manifest line (want '<Executable> <Suite>'): {line!r}")
        suites.append(fields[1])
    return suites


def read_timings(path, column):
    idx = COLUMNS.index(column) + 1
    table = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            cells = line.rstrip("\n").split("\t")
            if len(cells) != len(COLUMNS) + 1:
                sys.exit(f"[ERROR] {path}: malformed row: {line!r}")
            if cells[idx] != "-":
                table[cells[0]] = int(cells[idx])
    return table


def plan(args):
    expected = read_manifest(args.expected)
    if not expected:
        sys.exit("[ERROR] the expected-suite list is empty")

    device_suites = read_list(args.vulkan_device_suites)
    stale = sorted(set(device_suites) - set(expected))
    if stale:
        sys.exit(f"[ERROR] {args.vulkan_device_suites} names suite(s) that are not expected here: {stale}")
    runner = os.environ.get(args.vulkan_runner_var, "")
    if runner not in ("", "true", "false"):
        sys.exit(f"[ERROR] {args.vulkan_runner_var}={runner!r}: must be 'true', 'false' or unset")
    excluded = [] if runner == "true" else sorted(device_suites)
    if excluded:
        print(f"::notice title=Vulkan-device suites NOT run::{len(excluded)} suite(s) need a Vulkan device and "
              f"this runner has none ({args.vulkan_runner_var} is not 'true'): {' '.join(excluded)}")
        expected = [n for n in expected if n not in excluded]
        if not expected:
            sys.exit("[ERROR] every expected suite needs a Vulkan device and this runner has none")
    dupes = sorted({n for n in expected if expected.count(n) > 1})
    if dupes:
        sys.exit(f"[ERROR] the expected-suite list names a suite twice: {dupes}")

    table = read_timings(args.timings, args.column)
    known = sorted(table.values())
    default = known[len(known) // 2] if known else 1
    unknown = sorted(n for n in expected if n not in table)
    if unknown:
        print(f"[INFO] {len(unknown)} suite(s) have no recorded {args.column} time, weighted at the median "
              f"{default}s: {' '.join(unknown)}")
    weight = {n: table.get(n, default) for n in expected}

    # LPT: heaviest first, each to the currently lightest shard. Ties broken by name and by shard index,
    # so the same inputs give the same plan on every runner and every re-run.
    loads = [0] * args.shards
    shards = [[] for _ in range(args.shards)]
    for name in sorted(expected, key=lambda n: (-weight[n], n)):
        i = min(range(args.shards), key=lambda k: (loads[k], k))
        shards[i].append(name)
        loads[i] += weight[name]

    union = [n for s in shards for n in s]
    if sorted(union) != sorted(expected) or len(set(union)) != len(union):
        sys.exit("[ERROR] shard plan does not cover the expected suites exactly once")

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "expected.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(sorted(expected)) + "\n")
    with open(os.path.join(args.out, "needs-vulkan-device.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(n + "\n" for n in excluded))
    for i, s in enumerate(shards, 1):
        with open(os.path.join(args.out, f"shard-{i}.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(s) + "\n")
        print(f"shard {i}/{args.shards}: {len(s):3d} suites, {loads[i - 1]:5d} recorded suite-seconds, "
              f"heaviest {s[0]} ({weight[s[0]]}s)")
    print(f"{len(expected)} expected suites, each in exactly one of {args.shards} shards")


def verify(args):
    expected = set(read_list(os.path.join(args.plan, "expected.txt")))
    excluded = set(read_list(os.path.join(args.plan, "needs-vulkan-device.txt")))
    if excluded & expected:
        sys.exit(f"[ERROR] the plan both expects and excludes {sorted(excluded & expected)}")
    planned = {}
    for entry in sorted(os.listdir(args.plan)):
        m = re.fullmatch(r"shard-(\d+)\.txt", entry)
        if not m:
            continue
        for name in read_list(os.path.join(args.plan, entry)):
            if name in planned:
                sys.exit(f"[ERROR] {name} is planned in shard {planned[name]} and shard {m.group(1)}")
            planned[name] = m.group(1)
    errors = []
    if set(planned) != expected:
        errors.append(f"plan != expected: missing {sorted(expected - set(planned))}, "
                      f"extra {sorted(set(planned) - expected)}")

    ran = {}
    for entry in sorted(os.listdir(args.reports)):
        m = re.search(r"shard-(\d+)$", entry)
        d = os.path.join(args.reports, entry)
        if not m or not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if f.endswith(".xml"):
                ran.setdefault(f[:-4], []).append(m.group(1))

    for name in sorted(expected):
        got = ran.get(name, [])
        if len(got) != 1:
            errors.append(f"{name}: {len(got)} reports (shards {got}), expected exactly 1")
        elif got[0] != planned.get(name):
            errors.append(f"{name}: ran in shard {got[0]}, planned in shard {planned.get(name)}")
    for name in sorted(set(ran) - expected):
        why = "needs a Vulkan device and was left out of the plan" if name in excluded else "is not an expected suite"
        errors.append(f"{name}: has a report but {why}")

    if errors:
        print("[ERROR] the shards did not run the expected suites exactly once:")
        for e in errors:
            print("  " + e)
        sys.exit(1)
    print(f"{len(expected)} expected suites, each reported by exactly one shard, the one it was planned in")
    if excluded:
        print(f"::notice title=Vulkan-device suites NOT run::{len(excluded)} suite(s) were left out of this plan "
              f"for want of a Vulkan device: {' '.join(sorted(excluded))}")


def timings(args):
    for f in sorted(os.listdir(args.reports), key=str.lower):
        if f.endswith(".xml"):
            t = float(ET.parse(os.path.join(args.reports, f)).getroot().get("time", "0"))
            print(f"{f[:-4]}\t{max(1, math.ceil(t))}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--expected", required=True)
    p.add_argument("--timings", required=True)
    p.add_argument("--column", required=True, choices=COLUMNS)
    p.add_argument("--shards", required=True, type=int)
    p.add_argument("--out", required=True)
    p.add_argument("--vulkan-device-suites", required=True)
    p.add_argument("--vulkan-runner-var", required=True)
    v = sub.add_parser("verify")
    v.add_argument("--plan", required=True)
    v.add_argument("--reports", required=True)
    t = sub.add_parser("timings")
    t.add_argument("--reports", required=True)
    args = ap.parse_args()
    if args.cmd == "plan" and args.shards < 1:
        sys.exit("[ERROR] --shards must be at least 1")
    {"plan": plan, "verify": verify, "timings": timings}[args.cmd](args)


if __name__ == "__main__":
    main()
