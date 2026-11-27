#!/usr/bin/env python3
"""What is pushed but not in dev (owner 10-22, trunk-based).

The PT shot fix was written twice because ce3146917 sat in int/w1021 for a day and a half while dev lacked it.
This lists every origin task/* and int/* branch with commits dev does not have (newest first, > 24 h flagged),
and every Setup-level change dev took since this machine last looked (submodules, ThirdParty, the root CMake files):
a tree that has not updated its submodules and re-configured after one builds against the old copy.

Run at SessionStart (hook) and before any fix. `--quiet-ok` prints nothing when all is landed. `--hook`: read the
hook's JSON from stdin and print nothing inside an agent (an agent works in the tree its brief names; this list is
the lead's). At session start the list is limits.NOT_IN_DEV_LINES branches, the ones past 24 h first; `--all` for all.
"""
import json
import os
import subprocess
import sys
import time

DAY = 24 * 3600
RECENT_DAYS = 14  # older unlanded branches are abandoned work, not a fix to look for
SETUP_PATHS = [".gitmodules", "ThirdParty", "CMakeLists.txt", "CMakePresets.json"]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import limits  # noqa: E402


def git(*args, check=False):
    r = subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode != 0:
        raise RuntimeError(r.stderr.strip())
    return r.stdout


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if "--hook" in sys.argv:
        try:
            data = json.load(sys.stdin)
        except (ValueError, OSError):
            data = {}
        if data.get("agent_id") or data.get("agent_type"):
            return 0
    root = git("rev-parse", "--show-toplevel").strip()
    if not root:
        return 0
    os.chdir(root)
    try:
        subprocess.run(["git", "fetch", "-q", "origin"], timeout=10, capture_output=True)
    except subprocess.TimeoutExpired:
        print("[not_in_dev] git fetch timed out - lists below are from the last fetch")
    now = time.time()
    lines = []
    refs = git("for-each-ref", "--format=%(refname:short)", "refs/remotes/origin/task", "refs/remotes/origin/int")
    for ref in refs.split():
        log = git("log", "--format=%ct %h %s", f"origin/dev..{ref}")
        commits = [l.split(" ", 2) for l in log.splitlines() if l.strip()]
        if not commits:
            continue
        newest = int(commits[0][0])
        if now - newest > RECENT_DAYS * DAY:
            continue
        oldest = int(commits[-1][0])
        age_h = (now - oldest) / 3600
        flag = "  > 24 h, LAND IT" if age_h > 24 else ""
        lines.append((newest, f"  {ref[len('origin/'):]}: {len(commits)} commit(s), oldest {age_h:.0f} h - "
                              f"{commits[0][1]} {commits[0][2][:60]}{flag}"))
    if lines:
        print(f"[not_in_dev] pushed, NOT in dev ({len(lines)} branch(es)) - a fix may already be here:")
        ordered = [l for _, l in sorted(lines, key=lambda x: ("LAND IT" in x[1], x[0]), reverse=True)]
        cap = limits.NOT_IN_DEV_LINES
        for l in (ordered if "--all" in sys.argv else ordered[:cap]):
            print(l)
        if len(ordered) > cap and "--all" not in sys.argv:
            print(f"  ... +{len(ordered) - cap} more: python .claude/tools/not_in_dev.py --all")

    mark = os.path.join(git("rev-parse", "--git-common-dir").strip(), "not_in_dev_last_dev")
    dev = git("rev-parse", "origin/dev").strip()
    last = open(mark).read().strip() if os.path.exists(mark) else ""
    if last and last != dev and git("cat-file", "-t", last).strip() == "commit":
        changed = git("diff", "--name-only", f"{last}..{dev}", "--", *SETUP_PATHS).split()
        if changed:
            print(f"[not_in_dev] Setup-level changes in dev since {last[:9]} - update submodules and re-configure (cmake preset) in every tree:")
            for f in changed[:20]:
                print(f"  {f}")
    with open(mark, "w") as fh:
        fh.write(dev)
    if not lines and "--quiet-ok" not in sys.argv:
        print("[not_in_dev] every recent task/* and int/* commit is in dev")
    return 0


if __name__ == "__main__":
    sys.exit(main())
