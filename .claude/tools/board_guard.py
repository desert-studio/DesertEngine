#!/usr/bin/env python3
"""board_guard — the Notion board's side of the pipeline. Three jobs, nothing else:

  mirror   every Notion call of the `board` agent is mirrored into limits.BOARD_STATE (code -> status, page -> code),
           and a card update takes that code out of the pipeline's board queue (pipeline.json "board_queue");
  «Сделано» a card reaches it only with «Коммит» and «В dev» = __YES__ in the same call (owner 10-22);
  Stop     the lead's turn does not end while the hand-in debt is real: stopped writer runs nobody accepted, a board
           queue of limits.BOARD_BATCH updates (or older than limits.BOARD_MAX_AGE_H), or fewer live agents than the
           owner's floor while launchable work exists.

Launch rules, push rules and the run registry live in agent_guard.py; the numbers in limits.py; who runs in pipeline.py.
The board is written in batches: `python .claude/tools/studio.py board` prints one request for the `board` agent.
"""
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import limits
import pipeline as P

for _stream in (sys.stdin, sys.stdout, sys.stderr):
    _stream.reconfigure(encoding="utf-8")

CODE_RE = r"[A-Z][A-Z0-9]*(?:-[A-Z0-9+]+)*"
TITLE_RE = re.compile(r'"id"\s*:\s*"([0-9a-f-]{32,36})"\s*,\s*"title"\s*:\s*"(' + CODE_RE + r')\s+·')
CREATED_RE = re.compile(r'"id"\s*:\s*"([0-9a-f-]{32,36})"(?:(?!"id"\s*:).)*?"Код"\s*:\s*"(' + CODE_RE + r')"', re.S)
BOARD = "collection://386f5e0e-e749-479b-83a7-59e03ef9b8b2"
TAIL_STATUSES = ("В работе", "Готово к старту", "На проверке", "Заблокировано")
TAILS_SQL = ("SELECT url,\"Код\",\"Статус\",\"Зона\",\"Приоритет\",substr(\"Кто ведёт\",1,60) AS who,substr(\"Обновлено\",1,10) AS upd "
             f"FROM \"{BOARD}\" WHERE \"Статус\" IN ('В работе','Готово к старту','На проверке','Заблокировано') -- TAILS")
FOREIGN = "Mac" if sys.platform == "win32" else "Windows"
PRIO_ORDER = {"Владелец назвал": 0, "Высокий": 1, "Обычный": 2}


def load():
    try:
        with open(limits.BOARD_STATE, encoding="utf-8") as f:
            s = json.load(f)
    except (OSError, ValueError):
        s = {}
    for k in ("pages", "writes", "board"):
        s.setdefault(k, {})
    s.setdefault("board_t", 0)
    s.setdefault("session_t", 0)
    return s


def save(s):
    os.makedirs(limits.STUDIO_HOME, exist_ok=True)
    tmp = "%s.%d.tmp" % (limits.BOARD_STATE, os.getpid())  # per-process: two hooks sharing one tmp interleave
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(s, f)  # ASCII on purpose: the file is read back under any locale
    os.replace(tmp, limits.BOARD_STATE)


def norm(page_id):
    return (page_id or "").replace("-", "").lower()[-32:]


def strings(obj):
    """Every string inside a tool response, raw (an MCP result is text blocks holding JSON)."""
    if isinstance(obj, str):
        yield obj
    elif isinstance(obj, dict):
        for v in obj.values():
            yield from strings(v)
    elif isinstance(obj, list):
        for v in obj:
            yield from strings(v)


def emit(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False))
    sys.exit(0)


def foreign(row):
    return (row.get("who") or "").lstrip().startswith(FOREIGN)


def tails(s):
    return {c: r for c, r in s["board"].items() if r.get("status") in TAIL_STATUSES and not foreign(r)}


def ready_cards(s, n=3):
    """Cards «Готово к старту» of this side, the owner's first: the next work when the pipeline itself is empty."""
    rows = [(PRIO_ORDER.get(r.get("prio"), 3), r.get("upd") or "", c) for c, r in tails(s).items()
            if r.get("status") == "Готово к старту"]
    return [c for _, _, c in sorted(rows)[:n]]


def ingest_rows(s, resp, full, paged=False):
    """Mirror board rows («Код» + «Статус») from a query response; a full TAILS query also drops closed tails."""
    seen, partial = set(), paged
    for text in resp:
        try:
            obj = json.loads(text)
        except ValueError:
            continue
        if not isinstance(obj, dict):
            continue
        partial |= bool(obj.get("has_more"))
        for row in obj.get("results") or []:
            code, status = row.get("Код"), row.get("Статус")
            if not code or not status:
                continue
            seen.add(code)
            if row.get("url"):  # a later update-page by page id mirrors onto this code
                s["pages"][norm(row["url"])] = code
            cur = s["board"].setdefault(code, {})
            cur["status"] = status
            for src, dst in (("Зона", "zone"), ("Приоритет", "prio"), ("who", "who"), ("Кто ведёт", "who"), ("upd", "upd")):
                if row.get(src) is not None:
                    cur[dst] = str(row[src])[:60]
    if full and not partial:  # a paged answer has not seen every tail: closing the unseen would lie
        for code in list(s["board"]):
            if code not in seen and s["board"][code].get("status") in TAIL_STATUSES:
                s["board"][code]["status"] = "закрыта"
    if full:
        s["board_t"] = time.time()


def written(codes):
    """The board now holds these codes' state: their queued updates are done."""
    if not codes:
        return
    with P.locked() as p:
        p["board_queue"] = [q for q in p["board_queue"] if q.get("code") not in codes]


def mirror(s, tool, tin, response):
    resp = "\n".join(strings(response))
    for pid, code in TITLE_RE.findall(resp) + CREATED_RE.findall(resp):
        s["pages"][norm(pid)] = code
    now, done = time.time(), set()
    if tool.endswith("notion-create-pages"):
        for page in tin.get("pages") or []:
            props = page.get("properties") or {}
            code = props.get("Код")
            if code:
                s["writes"][code] = now
                s["board"][code] = {"status": props.get("Статус") or "Бэклог", "zone": props.get("Зона"),
                                    "prio": props.get("Приоритет"), "who": str(props.get("Кто ведёт") or "")[:60], "new": now}
                done.add(code)
    elif tool.endswith("notion-update-page"):
        props = tin.get("properties") or {}
        if props.get("Код"):  # a card re-coded by an update is that code from now on
            s["pages"][norm(tin.get("page_id"))] = props["Код"]
        code = s["pages"].get(norm(tin.get("page_id")))
        if code:
            s["writes"][code] = now
            row = s["board"].setdefault(code, {})
            if props.get("Статус"):
                row["status"] = props["Статус"]
            if props.get("Кто ведёт"):
                row["who"] = str(props["Кто ведёт"])[:60]
            done.add(code)
    elif tool.endswith("notion-query-data-sources"):
        sql = json.dumps(tin, ensure_ascii=False)
        ingest_rows(s, list(strings(response)), "-- TAILS" in sql, "OFFSET" in sql.upper())
    save(s)
    written(done)


def session_start(s):
    """At most two lines: the pipeline digest (studio.py digest) is the lead's view; this is only the board's."""
    fresh = s["board_t"] > 0 and time.time() - s["board_t"] < limits.BOARD_MAX_AGE_H * 3600
    s["session_t"] = time.time()
    save(s)
    t = tails(s)
    by = {}
    for r in t.values():
        by[r.get("status")] = by.get(r.get("status"), 0) + 1
    owner = sorted((c for c, r in t.items() if r.get("prio") == "Владелец назвал"), key=lambda c: t[c].get("upd") or "")
    line = (f"[board] открыто {len(t)}: " + "; ".join(f"{k} {v}" for k, v in sorted(by.items())) +
            (f"; «Владелец назвал» {len(owner)}: {' '.join(owner[:8])}" + (" …" if len(owner) > 8 else "") if owner else ""))
    if not fresh:
        line += ("\n[board] зеркало старше limits.BOARD_MAX_AGE_H — обнови его ВМЕСТЕ с первой пачкой записей: "
                 "python .claude/tools/studio.py board (один запрос агенту board).")
    print(line)


def stop_reason(p, s):
    """Why the lead's turn may not end, or None."""
    waiting = P.unaccepted(p)
    if waiting:
        return ("сдали и не приняты: " + " ".join(waiting[:8]) + ". Приём — одна команда на заход: "
                "python .claude/tools/studio.py accept <КОД> [done|wip|stuck|refused] (она же ставит запись доски в очередь).")
    n, age = P.board_debt(p)
    if n >= limits.BOARD_BATCH or (n and age > limits.BOARD_MAX_AGE_H):
        return (f"в очереди доски {n} записей, старейшая {age:.1f} ч (limits.BOARD_BATCH / BOARD_MAX_AGE_H): "
                "python .claude/tools/studio.py board → один запуск агента board на всю пачку.")
    night = P.night(p)
    need = limits.NIGHT_MIN_AGENTS if night else P.floor()
    busy = len(P.live_runs(p)) + (1 if P.landing(p) else 0)
    if busy < need:
        work = P.next_work(p) or ready_cards(s)
        if work:
            return (f"занято {busy} < пол {need}{' (ночной режим)' if night else ''} (~/.claude/min_agents; пауза владельца = 0). "
                    f"Следующее: {' '.join(work[:4])} — запусти в этом же ответе (python .claude/tools/studio.py digest).")
    return None


def main():
    s = load()
    if "--session-start" in sys.argv:
        session_start(s)
        sys.exit(0)
    try:
        data = json.load(sys.stdin)
    except ValueError:
        sys.exit(0)
    event, tool, tin = data.get("hook_event_name"), data.get("tool_name") or "", data.get("tool_input") or {}

    if event == "Stop":
        if data.get("stop_hook_active") or data.get("agent_id"):
            sys.exit(0)
        why = stop_reason(P.load(), s)
        if why:
            emit({"decision": "block", "reason": "[board_guard] " + why})
        sys.exit(0)

    if not tool.startswith("mcp__notion__"):
        sys.exit(0)
    if event == "PreToolUse" and tool.endswith("notion-update-page"):
        props = tin.get("properties") or {}
        if props.get("Статус") == "Сделано" and (not str(props.get("Коммит") or "").strip() or props.get("В dev") != "__YES__"):
            emit({"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny", "permissionDecisionReason":
                  "[board_guard] «Сделано» только с «Коммит» (SHA) и «В dev» = __YES__ в том же вызове (trunk-based, владелец "
                  "10-22). Не в dev — «На проверке»."}})
    elif event == "PostToolUse":
        mirror(s, tool, tin, data.get("tool_response"))
    sys.exit(0)


if __name__ == "__main__":
    main()
