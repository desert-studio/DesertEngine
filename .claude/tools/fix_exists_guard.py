#!/usr/bin/env python3
"""PreToolUse on Edit/Write: "a fix may already exist" (owner 10-22, trunk-based).

The first time a session edits an engine source file (Desert/, Editor/, Tools/), list the commits pushed to
origin task/* and int/* - not yet in dev - that touch the same file. The PT shot fix was written twice because
ce3146917 changed SceneRenderer.cpp in int/w1021 a day before the same change was made again from dev.

It does not refuse: it tells the session to read those commits first (cherry-pick an existing fix, never
re-write it). The file -> commits map is cached per fetch (keyed by the origin refs), so only the first edit
after a fetch pays for the git log walk.
"""
import hashlib
import json
import os
import subprocess
import sys
import time

WATCHED = ("Desert/", "Editor/", "Tools/")
RECENT_DAYS = 14


def git(*args):
    r = subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.stdout


def build_map(cache_path, key):
    files = {}
    since = f"--since={RECENT_DAYS}.days"
    refs = git("for-each-ref", "--format=%(refname:short)", "refs/remotes/origin/task", "refs/remotes/origin/int")
    for ref in refs.split():
        commit = None
        for line in git("log", since, "--format=\x01%h %s", "--name-only", f"origin/dev..{ref}").splitlines():
            if line.startswith("\x01"):
                commit = f"{line[1:][:110]} [{ref[len('origin/'):]}]"
            elif line.strip() and commit:
                bucket = files.setdefault(line.strip(), [])
                if commit not in bucket:
                    bucket.append(commit)
    with open(cache_path, "w", encoding="utf-8") as fh:
        json.dump({"key": key, "files": files}, fh)
    return files


def main():
    data = json.load(sys.stdin)
    if data.get("tool_name") not in ("Edit", "Write", "MultiEdit"):
        return 0
    path = (data.get("tool_input") or {}).get("file_path") or ""
    root = git("rev-parse", "--show-toplevel").strip()
    if not path or not root:
        return 0
    rel = os.path.relpath(os.path.abspath(path), os.path.abspath(root)).replace("\\", "/")
    if not rel.startswith(WATCHED):
        return 0

    common = git("rev-parse", "--git-common-dir").strip()
    seen_path = os.path.join(common, "fix_exists_seen.json")
    seen = {}
    if os.path.exists(seen_path):
        try:
            seen = json.load(open(seen_path, encoding="utf-8"))
        except ValueError:
            seen = {}
    session = data.get("session_id") or "?"
    # Once per file per session; sessions older than a day are dropped.
    now = time.time()
    seen = {s: v for s, v in seen.items() if now - v.get("t", 0) < 86400}
    entry = seen.setdefault(session, {"t": now, "files": []})
    if rel in entry["files"]:
        return 0
    entry["files"].append(rel)
    json.dump(seen, open(seen_path, "w", encoding="utf-8"))

    key = hashlib.sha1(git("for-each-ref", "--format=%(objectname)", "refs/remotes/origin").encode()).hexdigest()
    cache_path = os.path.join(common, "fix_exists_map.json")
    files = None
    if os.path.exists(cache_path):
        try:
            cache = json.load(open(cache_path, encoding="utf-8"))
            files = cache["files"] if cache.get("key") == key else None
        except (ValueError, KeyError):
            files = None
    if files is None:
        files = build_map(cache_path, key)
    hits = files.get(rel)
    if not hits:
        return 0
    text = (f"[fix_exists] {rel} is changed by {len(hits)} pushed commit(s) NOT in dev - a fix may already exist. "
            f"Read them before editing (git show <sha> -- {rel}); if one is the fix, cherry-pick it instead of "
            f"re-writing it (owner 10-22, trunk-based):\n  " + "\n  ".join(hits[:8]))
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse", "additionalContext": text}}))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # a broken advisory hook must not block edits
        print(f"[fix_exists] skipped: {exc}", file=sys.stderr)
        sys.exit(0)
