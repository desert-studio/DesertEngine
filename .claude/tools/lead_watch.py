#!/usr/bin/env python3
"""Lead spend watch (owner 2026-10-05: "следи всегда, чтобы я не напоминал").

The lead is ~3/4 of the spend, and 84 % of the lead's cost is re-reading its own context on every request, so
the levers are the NUMBER of lead wake-ups and the LENGTH of the lead context. This hook reads the lead's
transcript incrementally after every tool call and puts a warning into the lead's context the moment a waste
pattern appears; every REPORT_EVERY requests it reports the running totals and appends them to
~/.claude/lead-spend.csv. Sub-agents are ignored (agent_guard watches them).

Patterns:
  idle wake   — a turn woken by a notification / agent message that made no tool call (pure cost);
  stale watch — a notification of a background task this session never started (a waiter left by an older
                session: find it with ps and kill it);
  long context — the last request's context above CONTEXT_LIMIT: write the handover and ask for /clear.
Cost units as ledger.py: 5*out + 1.25*new input + 0.1*cache reads.
"""
import csv, json, os, re, sys, time

STATE_DIR = os.path.expanduser("~/.claude/lead-watch")
CSV = os.path.expanduser("~/.claude/lead-spend.csv")
REPORT_EVERY = 20
CONTEXT_LIMIT = 250_000
NOTIFY = re.compile(r"<task-notification>|Another Claude session sent a message|<agent-message")
TASK_ID = re.compile(r"<task-id>([A-Za-z0-9]+)</task-id>")
STARTED = re.compile(r"(?:running in background with ID: |agentId: )([A-Za-z0-9]+)")


def text_of(content):
    if isinstance(content, str):
        return content
    out = []
    for b in content or []:
        if isinstance(b, dict):
            if b.get("type") == "text":
                out.append(b.get("text", ""))
            elif b.get("type") == "tool_result":
                c = b.get("content")
                out.append(text_of(c) if not isinstance(c, str) else c)
    return "\n".join(out)


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def main():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        return
    if data.get("agent_id"):
        return  # a sub-agent's call
    tpath = data.get("transcript_path")
    sid = data.get("session_id") or "?"
    if not tpath or not os.path.exists(tpath):
        return
    os.makedirs(STATE_DIR, exist_ok=True)
    spath = os.path.join(STATE_DIR, sid + ".json")
    st = load(spath) or {"offset": 0, "req": {}, "n": 0, "out": 0, "new": 0, "read": 0, "ctx": 0,
                          "turn_notify": False, "turn_tools": 0, "turn_tid": "", "idle": 0, "stale": 0,
                          "started": [], "pending": [], "last_report_n": 0}
    started = set(st["started"])
    pending = st["pending"]
    size = os.path.getsize(tpath)
    if size < st["offset"]:
        st["offset"] = 0
    with open(tpath, encoding="utf-8", errors="replace") as f:
        f.seek(st["offset"])
        for line in f:
            if not line.endswith("\n"):
                break  # a half-written line: read it next time
            st["offset"] += len(line.encode("utf-8"))
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            kind = rec.get("type")
            msg = rec.get("message") or {}
            if kind == "user":
                content = msg.get("content")
                is_result = isinstance(content, list) and any(
                    isinstance(b, dict) and b.get("type") == "tool_result" for b in content)
                txt = text_of(content)
                for m in STARTED.finditer(txt):
                    started.add(m.group(1))
                if is_result:
                    continue
                # a new turn begins: close the previous one
                if st["turn_notify"] and st["turn_tools"] == 0:
                    st["idle"] += 1
                    pending.append(f"idle wake (no tool call) after {st['turn_tid'] or 'an agent message'}")
                st["turn_notify"] = bool(NOTIFY.search(txt))
                st["turn_tools"] = 0
                tid = TASK_ID.search(txt)
                st["turn_tid"] = tid.group(1) if tid else ""
                if tid and tid.group(1) not in started:
                    st["stale"] += 1
                    pending.append(f"stale waiter {tid.group(1)} (not started by this session: ps aux | grep it, kill)")
            elif kind == "assistant":
                rid = rec.get("requestId") or rec.get("uuid")
                u = msg.get("usage") or {}
                for b in msg.get("content") or []:
                    if isinstance(b, dict) and b.get("type") == "tool_use":
                        st["turn_tools"] += 1
                if u and rid:
                    prev = st["req"].get(rid)
                    if prev is None:
                        st["n"] += 1
                    else:
                        st["out"] -= prev[0]; st["new"] -= prev[1]; st["read"] -= prev[2]
                    new = u.get("input_tokens", 0) + u.get("cache_creation_input_tokens", 0)
                    rec3 = [u.get("output_tokens", 0), new, u.get("cache_read_input_tokens", 0)]
                    st["req"][rid] = rec3
                    st["out"] += rec3[0]; st["new"] += rec3[1]; st["read"] += rec3[2]
                    st["ctx"] = new + rec3[2]
    # keep only the last few request ids (usage repeats only within a request)
    if len(st["req"]) > 50:
        st["req"] = dict(list(st["req"].items())[-50:])
    st["started"] = sorted(started)[-500:]
    units = 5 * st["out"] + 1.25 * st["new"] + 0.1 * st["read"]
    lines = []
    if pending:
        lines += pending[-5:]
        pending = []
    if st["ctx"] > CONTEXT_LIMIT and not st.get("ctx_warned"):
        st["ctx_warned"] = True
        lines.append(f"context {st['ctx']//1000}k > {CONTEXT_LIMIT//1000}k: every request re-reads it — write the handover and ask the owner for /clear at the next phase boundary")
    if st["n"] - st["last_report_n"] >= REPORT_EVERY:
        st["last_report_n"] = st["n"]
        read_pct = round(100 * 0.1 * st["read"] / units) if units else 0
        lines.append(f"{st['n']} requests, {units/1e6:.2f}M units ({read_pct}% cache re-read), context {st['ctx']//1000}k, "
                     f"idle wakes {st['idle']}, stale waiters {st['stale']}")
        new_file = not os.path.exists(CSV)
        with open(CSV, "a", newline="") as fh:
            w = csv.writer(fh)
            if new_file:
                w.writerow(["time", "session", "requests", "units", "out", "new", "cache_read", "context", "idle_wakes", "stale_waiters"])
            w.writerow([time.strftime("%Y-%m-%d %H:%M"), sid, st["n"], int(units), st["out"], st["new"], st["read"], st["ctx"], st["idle"], st["stale"]])
    st["pending"] = pending
    with open(spath, "w") as f:
        json.dump(st, f)
    if lines:
        msg = "[lead_watch] " + " | ".join(lines) + " — fix the cause now (LEAD_PROTOCOL §000d, memory lead-is-the-spend)."
        print(json.dumps({"hookSpecificOutput": {"hookEventName": data.get("hook_event_name", "PostToolUse"),
                                                 "additionalContext": msg}}))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # a watch must never break the lead's tool call
        print(f"lead_watch: {e}", file=sys.stderr)
