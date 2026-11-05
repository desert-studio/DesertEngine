#!/usr/bin/env python3
"""PreToolUse gate for builds (owner 2026-11-05: «надо как-то записать оптимизацию в протокол, чтобы не делаем по
1000 сборок на день»). LEAD_PROTOCOL §СБОРКА.

A build is one call of .claude/tools/build_land.sh <land tree> <log> [scn]. The gate allows it when
  - nothing builds right now (the previous log has no <log>.rc and was written in the last 5 minutes = running), and
  - the previous build of the tree failed (compile-fix loop), or there is none, or at least MIN_MERGES merge
    commits landed since its HEAD, or the command carries BUILD_WHY="<reason>" (at most WHY_PER_DAY a day).
Agents never build. Any other build entry (BuildWindows.bat, cmake --build, MSBuild, ninja, build_low*.sh) is
refused with the one allowed form. Every allowed build is written to LEDGER with its HEAD, log and reason.
"""
import json
import os
import re
import subprocess
import sys
import time

LAND_TREE = os.environ.get("DESERT_LAND_TREE", "F:/DesertEngine")
LEDGER = os.environ.get("BUILD_GATE_LEDGER", "C:/aftmp/build_ledger.jsonl")
MIN_MERGES = 2
WHY_PER_DAY = 3
RUNNING_SECONDS = 300

ENTRY = re.compile(r"(?:^|[;&|(]\s*|\bbash\s+|\bsh\s+)(?:\S*/)?build_land\.sh\s+(\S+)\s+(\S+)")
OTHER = re.compile(r"(BuildWindows\.bat|cmake\s+--build|MSBuild(\.exe)?[\"']?\s|(^|[;&|(]\s*)ninja\s|"
                   r"build_low(_run)?\.sh\s)", re.I)
READERS = re.compile(r"^\s*(cat|head|tail|grep|sed\s+-n|less|wc|ls|git\s+(log|show|diff|grep))\b")
WHY = re.compile(r"BUILD_WHY=(\"[^\"]+\"|'[^']+'|\S+)")


def emit(decision, text):
    if decision == "deny":
        out = {"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny",
                                      "permissionDecisionReason": text}}
    else:
        out = {"hookSpecificOutput": {"hookEventName": "PreToolUse", "additionalContext": text}}
    sys.stdout.write(json.dumps(out))
    sys.exit(0)


def norm(path):
    p = path.strip("\"'").replace("\\", "/").rstrip("/")
    m = re.match(r"^/([a-zA-Z])/(.*)$", p)
    if m:
        p = m.group(1) + ":/" + m.group(2)
    return p.lower()


def git(tree, *args):
    r = subprocess.run(["git", "-C", tree, *args], capture_output=True, text=True, timeout=20)
    return r.returncode, r.stdout.strip()


def ledger():
    rows = []
    try:
        with open(LEDGER, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    rows.append(json.loads(line))
    except (OSError, ValueError):
        pass
    return rows


def state_of(row):
    """'running' | 'failed' | 'ok' | 'unknown' for the build a ledger row started."""
    log = row.get("log", "")
    try:
        with open(log + ".rc", encoding="utf-8") as f:
            return "ok" if f.read().strip() == "0" else "failed"
    except OSError:
        pass
    try:
        if time.time() - os.path.getmtime(log) < RUNNING_SECONDS:
            return "running"
    except OSError:
        pass
    return "unknown"


def main():
    data = json.load(sys.stdin)
    if data.get("tool_name") not in ("Bash", "PowerShell"):
        sys.exit(0)
    cmd = (data.get("tool_input") or {}).get("command", "")
    if re.match(r"^\s*git\s", cmd) or "<<" in cmd:
        sys.exit(0)  # a commit message or a heredoc that only mentions a build is not one
    entry = ENTRY.search(cmd)
    if not entry and (not OTHER.search(cmd) or READERS.match(cmd)):
        sys.exit(0)
    form = "bash F:/DesertEngine/.claude/tools/build_land.sh F:/DesertEngine C:/aftmp/build_<name>.log [scn]"
    if data.get("agent_id"):
        emit("deny", "[build_gate] агенты не собирают (LEAD_PROTOCOL §СБОРКА): код + push, сборку делает лид "
                     "одной сборкой пачки.")
    if not entry:
        emit("deny", "[build_gate] сборка только одной командой: " + form + " (LEAD_PROTOCOL §СБОРКА).")
    tree, log = norm(entry.group(1)), entry.group(2).strip("\"'")
    if tree != norm(LAND_TREE):
        emit("deny", f"[build_gate] собирается только дерево пачки {LAND_TREE}: ветки агентов сливаются в него, "
                     "сборка одна на пачку (LEAD_PROTOCOL §СБОРКА).")
    rows = ledger()
    today = time.strftime("%Y-%m-%d")
    today_rows = [r for r in rows if r.get("day") == today]
    for r in rows[-6:]:
        if state_of(r) == "running":
            emit("deny", f"[build_gate] сборка уже идёт ({r.get('log')}); одна сборка за раз.")
    rc, head = git(LAND_TREE, "rev-parse", "HEAD")
    if rc != 0:
        emit("deny", f"[build_gate] не прочитан HEAD {LAND_TREE}.")
    prev = next((r for r in reversed(rows) if norm(r.get("tree", "")) == tree), None)
    why_m = WHY.search(cmd)
    why = why_m.group(1).strip("\"'") if why_m else ""
    reason = ""
    if prev is None:
        reason = "первая сборка дерева"
    else:
        st = state_of(prev)
        if st in ("failed", "unknown"):
            reason = f"прошлая сборка {st}: цикл исправления"
        else:
            rc, anc = git(LAND_TREE, "merge-base", "--is-ancestor", prev["head"], head)
            merges = 0
            if rc == 0:
                _, n = git(LAND_TREE, "rev-list", "--merges", "--count", f"{prev['head']}..{head}")
                merges = int(n or 0)
            if rc != 0:
                reason = "HEAD не потомок прошлой сборки"
            elif merges >= MIN_MERGES:
                reason = f"влито {merges} веток с прошлой сборки"
            elif why:
                used = sum(1 for r in today_rows if r.get("why"))
                if used >= WHY_PER_DAY:
                    emit("deny", f"[build_gate] BUILD_WHY уже {used} раз за {today} (предел {WHY_PER_DAY}). "
                                 "Слей ещё ветки пачки и собери их вместе.")
                reason = "BUILD_WHY: " + why
            else:
                emit("deny", f"[build_gate] с прошлой удачной сборки ({prev['head'][:9]}) влито {merges} веток; "
                             f"нужно >= {MIN_MERGES}. Сначала весь код пачки, потом одна сборка. Исключение — "
                             f"BUILD_WHY=\"<причина>\" перед командой (не больше {WHY_PER_DAY} в день).")
    try:
        os.makedirs(os.path.dirname(LEDGER), exist_ok=True)
        with open(LEDGER, "a", encoding="utf-8") as f:
            f.write(json.dumps({"t": time.time(), "day": today, "tree": LAND_TREE, "head": head, "log": log,
                                "why": why, "reason": reason}, ensure_ascii=False) + "\n")
    except OSError:
        pass
    emit("allow", f"[build_gate] сборка #{len(today_rows) + 1} за {today}: {reason}.")


if __name__ == "__main__":
    main()
