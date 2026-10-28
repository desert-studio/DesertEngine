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
  long context — above CLEAR_AT every owner prompt (and every CLEAR_EVERY requests) carries the /clear reminder:
                write the handover and ask the owner for /compact (owner 10-28: compact вместо clear);
  digging     — the lead's own log/code digging fills its context (10-20: 43 shell reads + 40 searches = 39 % of the
                lead's spend): a tool result over BIG_RESULT chars, or more than DIG_PER_TURN reads/searches in a turn;
  big schema  — ToolSearch loading a large deferred schema (notion-query-data-sources ~20k) into the lead: it stays
                in every later request (10-20: ToolSearch 17 % of the lead);
  long wait   — a foreground call with a timeout over the cache TTL keeps the lead open while nothing happens.
Cost units as ledger.py: 5*out + 1.25*new input + 0.1*cache reads.
"""
import csv, json, os, re, sys, time

# Windows: the hook event and our messages are UTF-8, not the console code page (cp1251 lost «Код» on 10-20).
for _stream in (sys.stdin, sys.stdout, sys.stderr):
    _stream.reconfigure(encoding="utf-8")

STATE_DIR = os.path.expanduser("~/.claude/lead-watch")
CSV = os.path.expanduser("~/.claude/lead-spend.csv")
REPORT_EVERY = 20
CONTEXT_LIMIT = 250_000
CLEAR_AT = 150_000
CLEAR_EVERY = 10
BIG_RESULT = 6_000
DIG_PER_TURN = 6
DIG_TOOLS = re.compile(r"^\s*(cat|head|tail|sed|grep|rg|awk|find|ls|git\s+(log|show|diff|grep))")
BIG_SCHEMAS = re.compile(r"notion-query-data-sources|notion-query-meeting-notes|notion-create-database")
LONG_WAIT_MS = 300_000
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
    st.setdefault("turn_dig", 0); st.setdefault("last_clear_n", 0)
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
                    size = sum(len(text_of(b.get("content"))) for b in content
                               if isinstance(b, dict) and b.get("type") == "tool_result")
                    if size > BIG_RESULT:
                        pending.append(f"tool result {size//1000}k chars entered the lead context and is re-read on every "
                                       "later request — print a 3-5 line digest from a script, or give the digging to a haiku agent")
                    continue
                # a new turn begins: close the previous one
                if st["turn_notify"] and st["turn_tools"] == 0:
                    st["idle"] += 1
                    pending.append(f"idle wake (no tool call) after {st['turn_tid'] or 'an agent message'}")
                st["turn_notify"] = bool(NOTIFY.search(txt))
                st["turn_tools"] = 0
                st["turn_dig"] = 0
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
                        inp = b.get("input") or {}
                        name = b.get("name") or ""
                        if name in ("Read", "Grep", "Glob") or (name in ("Bash", "PowerShell") and DIG_TOOLS.match(str(inp.get("command") or ""))):
                            st["turn_dig"] += 1
                            if st["turn_dig"] == DIG_PER_TURN + 1:
                                pending.append(f"{DIG_PER_TURN}+ reads/searches by the lead in one turn — the lead coordinates; "
                                               "digging goes to a script that prints a digest or to a haiku Explore agent")
                        if name == "ToolSearch" and BIG_SCHEMAS.search(str(inp.get("query") or "")):
                            pending.append("ToolSearch loaded a large schema into the lead; it is re-read on every later request — "
                                           "run such queries from a sub-agent or a script")
                        if int(inp.get("timeout") or 0) > LONG_WAIT_MS and not inp.get("run_in_background"):
                            pending.append("foreground call longer than the cache TTL — run it in the background and end the turn")
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
    event = data.get("hook_event_name", "")
    if st["ctx"] > CLEAR_AT and (event == "UserPromptSubmit" or st["n"] - st["last_clear_n"] >= CLEAR_EVERY):
        st["last_clear_n"] = st["n"]
        lines.append(f"/COMPACT: context {st['ctx']//1000}k > {CLEAR_AT//1000}k — every request re-reads it; write the handover "
                     "(memory STATE FOR /clear) and ASK THE OWNER FOR /compact in this reply (owner 10-28: compact, not clear)")
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
