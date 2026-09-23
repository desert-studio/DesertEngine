#!/usr/bin/env python3
"""Where an agent's tokens went, read from its transcript rather than estimated.

Usage: agent_spend.py <agent transcript .jsonl> [more.jsonl ...]

The transcript is the subagent JSONL Claude Code writes under
~/.claude/projects/<project>/<session>/subagents/. It exists only on this machine and only
until the session directory is cleaned, so run this AT ACCEPTANCE, next to filling the
board's `Токены` field (LEAD_PROTOCOL.md 3.2).

WHAT EACH COLUMN MEANS, because the three are different costs:

  out     tokens the model WROTE on the turn that issued the call (reasoning + the call itself).
  added   tokens the call's RESULT added to the context: the uncached input of the NEXT request.
  rebuilt context that had been cached and had to be WRITTEN AGAIN on the next request because
          the cache expired in between. Agents run on a 5-minute cache TTL, so any call that keeps
          the agent waiting longer than that -- a build, a CI watch, a sleep -- makes the next turn
          re-pay the whole context at the cache-write price. MEASURED 2026-09-23 over the 19
          agents of that day: builds put ~34k tokens of OUTPUT into the context and cost ~8M in
          rebuilds. A first version of this tool blamed the log; it was the clock.
  carried added x the number of requests that came after it -- every token a result adds is
          re-read on every later turn, so a 20k-token log dumped early costs far more than
          its size.
  gap     median wall-clock seconds between issuing the call and the next request.

Requests are deduplicated by requestId: one response split into several content blocks
repeats its usage on each block, and summing them would count the same request twice.
"""
import json
import re
import sys
from collections import defaultdict

# Order matters: the first pattern that matches a Bash command names its category.
BASH_CATEGORIES = [
    ("build", re.compile(r"\bmake\b|premake5|Build\w*\.sh|build\.bat|msbuild|ccache")),
    ("run tests", re.compile(r"RunAllTests|/Tests/|_test\b|Debug/\w+Test|--gtest")),
    ("run editor", re.compile(r"RunEditor|--shot\b")),
    ("ci (gh)", re.compile(r"^\s*gh\b|\bgh (run|pr|api)\b")),
    ("git", re.compile(r"^\s*(cd [^;&]+&&\s*)?git\b")),
    ("search", re.compile(r"\b(grep|rg|ugrep|find)\b")),
    ("read (shell)", re.compile(r"\b(cat|sed -n|head|tail|less|wc)\b")),
]


# The LEADING word decides first: `grep ccache ci.yml` is a search, not a build, even though it
# names ccache. Only a command that does not start with a reader/searcher falls to the patterns.
LEADING_WORD = {
    "grep": "search", "rg": "search", "ugrep": "search", "find": "search",
    "cat": "read (shell)", "sed": "read (shell)", "head": "read (shell)", "tail": "read (shell)",
    "wc": "read (shell)", "ls": "read (shell)", "git": "git", "gh": "ci (gh)",
}
LEADING_SKIP = re.compile(r"^\s*((cd|export|[A-Z_]+=)\S*[^;&|]*(&&|;)\s*)*")


def categorize(block):
    name = block.get("name", "?")
    if name != "Bash":
        return name
    cmd = block.get("input", {}).get("command", "")
    first = LEADING_SKIP.sub("", cmd, count=1).split(None, 1)
    if first and first[0] in LEADING_WORD:
        return "Bash: " + LEADING_WORD[first[0]]
    for label, pattern in BASH_CATEGORIES:
        if pattern.search(cmd):
            return "Bash: " + label
    return "Bash: other"


def analyse(path):
    requests = {}  # requestId -> {"usage":..., "tools":[...], "order": n}
    order = 0
    with open(path) as f:
        for line in f:
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("type") != "assistant":
                continue
            msg = rec.get("message", {})
            rid = rec.get("requestId") or rec.get("uuid")
            entry = requests.get(rid)
            if entry is None:
                entry = {"usage": {}, "tools": [], "order": order}
                order += 1
                requests[rid] = entry
            if msg.get("usage"):
                entry["usage"] = msg["usage"]  # the last block carries the final count
            entry.setdefault("t0", rec.get("timestamp"))
            for block in msg.get("content") or []:
                if block.get("type") == "tool_use":
                    entry["tools"].append(categorize(block))

    seq = sorted(requests.values(), key=lambda e: e["order"])
    n = len(seq)
    rows = defaultdict(lambda: {"calls": 0, "out": 0, "added": 0, "rebuilt": 0, "carried": 0, "gaps": []})
    totals = {"out": 0, "new": 0, "read": 0, "rebuilt": 0}

    for i, entry in enumerate(seq):
        u = entry["usage"]
        new = u.get("input_tokens", 0) + u.get("cache_creation_input_tokens", 0)
        totals["out"] += u.get("output_tokens", 0)
        totals["new"] += new
        totals["read"] += u.get("cache_read_input_tokens", 0)

        tools = entry["tools"] or ["(no tool: final answer / pure reasoning)"]
        share = 1.0 / len(tools)
        added = rebuilt = 0
        gap = None
        if i + 1 < n:
            nxt = seq[i + 1]
            nu = nxt["usage"]
            next_new = nu.get("input_tokens", 0) + nu.get("cache_creation_input_tokens", 0)
            # Everything this request READ or WROTE to the cache should be a cache READ next time.
            # Whatever of it is not came back as a write: the cache expired in between.
            was_cached = u.get("cache_read_input_tokens", 0) + u.get("cache_creation_input_tokens", 0)
            rebuilt = min(next_new, max(0, was_cached - nu.get("cache_read_input_tokens", 0)))
            added = next_new - rebuilt
            gap = seconds_between(entry.get("t0"), nxt.get("t0"))
        totals["rebuilt"] += rebuilt
        remaining = n - (i + 2)  # requests that re-read it after it was added
        for t in tools:
            r = rows[t]
            r["calls"] += 1 if entry["tools"] else 0
            r["out"] += u.get("output_tokens", 0) * share
            r["added"] += added * share
            r["rebuilt"] += rebuilt * share
            r["carried"] += added * share * max(remaining, 0)
            if gap is not None and entry["tools"]:
                r["gaps"].append(gap)

    first_new = (seq[0]["usage"].get("input_tokens", 0) + seq[0]["usage"].get("cache_creation_input_tokens", 0)) if seq else 0
    return n, rows, totals, first_new


def seconds_between(a, b):
    from datetime import datetime
    try:
        fa = datetime.fromisoformat(a.replace("Z", "+00:00"))
        fb = datetime.fromisoformat(b.replace("Z", "+00:00"))
    except (AttributeError, ValueError):
        return None
    return (fb - fa).total_seconds()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    combined = defaultdict(lambda: {"calls": 0, "out": 0, "added": 0, "rebuilt": 0, "carried": 0, "gaps": []})
    for path in sys.argv[1:]:
        n, rows, totals, first_new = analyse(path)
        print(f"== {path}")
        print(f"requests {n}; output {totals['out']:,}; new input {totals['new']:,} "
              f"(of which the opening prompt+system {first_new:,}, re-cached after expiry {totals['rebuilt']:,}); "
              f"cache reads {totals['read']:,}")
        print_rows(rows)
        for name, r in rows.items():
            for k in r:
                combined[name][k] += r[k]  # lists concatenate, numbers add
    if len(sys.argv) > 2:
        print(f"== ALL {len(sys.argv) - 1} transcripts")
        print_rows(combined)


def print_rows(rows):
    # PRICE, NOT COUNT. A cache read costs a tenth of an input token, a cache write 1.25x, an output
    # token 5x -- the ratios Anthropic prices every Claude model at. Ranking by raw carried tokens would
    # overstate the cheap column tenfold; `cost` is in input-token units.
    def cost(r):
        return 5 * r["out"] + 1.25 * (r["added"] + r["rebuilt"]) + 0.1 * r["carried"]

    total = sum(cost(r) for r in rows.values()) or 1
    print(f"{'category':40} {'calls':>5} {'out':>9} {'added':>9} {'rebuilt':>10} {'carried':>12} "
          f"{'gap s':>6} {'cost':>11} {'share':>6}")
    for name, r in sorted(rows.items(), key=lambda kv: -cost(kv[1])):
        g = sorted(r["gaps"])
        med = f"{g[len(g) // 2]:.0f}" if g else "-"
        print(f"{name[:40]:40} {r['calls']:>5} {int(r['out']):>9,} {int(r['added']):>9,} {int(r['rebuilt']):>10,} "
              f"{int(r['carried']):>12,} {med:>6} {int(cost(r)):>11,} {100 * cost(r) / total:>5.1f}%")
    print()


if __name__ == "__main__":
    main()
