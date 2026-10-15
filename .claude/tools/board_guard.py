#!/usr/bin/env python3
"""board_guard — the Notion board is kept by a hook, not by the lead's memory.

Owner 10-09: «А карточку кто будет обновлять???? Почему я каждый раз тебя тыкаю?» — 7 agents were launched with no card
and a lead decision went only into an agent's REMAINDER. Rules (lead session only; sub-agents pass through):

1. Agent launch (not Explore) is refused until the task's card was written: notion-create-pages with «Код» = the code,
   or notion-update-page on a page whose title starts with «<CODE> ·». The code is read from the prompt
   («You are agent <CODE>» or /private/tmp/claude-501/<CODE>/BRIEF.md).
2. A working agent stopping (SubagentStop) leaves its code «to accept». Until its card is written after that moment,
   the lead may not launch or resume agents, and the lead's turn may not end (Stop is blocked once per turn).
3. Writing a lead decision into a REMAINDER.md gets a reminder: the decision goes into the card body.
4. A card set to «На проверке»/«Сделано» unblocks its dependents. Owner 10-14: «почему не сделан UI панели?» —
   SCR-DETAILS («Владелец назвал», «после SCR-PORT») lay in the backlog 5 days after SCR-PORT-2 was done. Until a
   notion-query-data-sources whose SQL names the code AND «Владелец назвал» is run, agent launches are refused and
   the turn may not end: every owner-named card waiting on that code is either launched or reported to the owner.

State: ~/.claude/board_guard_state.json {pages: {page_id: code}, writes: {code: t}, pending: {code: t}, agents: {id: code}}.
"""
import json
import os
import re
import sys
import time

STATE = os.path.expanduser("~/.claude/board_guard_state.json")
CODE_RE = r"[A-Z][A-Z0-9]*(?:-[A-Z0-9+]+)*"
TITLE_RE = re.compile(r'"id"\s*:\s*"([0-9a-f-]{32,36})"\s*,\s*"title"\s*:\s*"(' + CODE_RE + r')\s+·')
CREATED_RE = re.compile(r'"id"\s*:\s*"([0-9a-f-]{32,36})"(?:(?!"id"\s*:).)*?"Код"\s*:\s*"(' + CODE_RE + r')"', re.S)


def load():
    try:
        with open(STATE) as f:
            s = json.load(f)
    except (OSError, ValueError):
        s = {}
    for k in ("pages", "writes", "pending", "agents", "unblock"):
        s.setdefault(k, {})
    return s


def save(s):
    tmp = STATE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(s, f, ensure_ascii=False)
    os.replace(tmp, STATE)


def norm(page_id):
    return (page_id or "").replace("-", "").lower()[-32:]


def code_from_text(text):
    m = re.search(r"You are agent (" + CODE_RE + r")", text) or \
        re.search(r"/private/tmp/claude-501/(" + CODE_RE + r")/BRIEF\.md", text)
    return m.group(1) if m else None


def code_from_transcript(path):
    try:
        with open(path) as f:
            for _, line in zip(range(5), f):
                code = code_from_text(line)
                if code:
                    return code
    except OSError:
        pass
    return None


def strings(obj):
    """Every string inside a tool response, raw (an MCP result is text blocks holding JSON; dumping them escapes quotes)."""
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


def deny(reason):
    emit({"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny",
                                 "permissionDecisionReason": "[board_guard] " + reason}})


def unaccepted(s):
    return sorted(c for c, t in s["pending"].items() if s["writes"].get(c, 0) < t)


UNBLOCK_SQL = ("SELECT \"Код\",\"Статус\",\"Название\" FROM \"collection://386f5e0e-e749-479b-83a7-59e03ef9b8b2\" "
               "WHERE \"Приоритет\"='Владелец назвал' AND \"Статус\" IN ('Бэклог','Готово к старту','Заблокировано') "
               "-- после {codes}")


def unblock_reason(codes):
    return ("сдано " + ", ".join(codes) + " — проверь, что ждало этого среди карточек «Владелец назвал»: "
            "notion-query-data-sources, SQL с кодом и «Владелец назвал», напр. " + UNBLOCK_SQL.format(codes=" ".join(codes)) +
            " (затем открой карточки, где в теле «после <КОД>»). Ждавшее — запустить сразу или назвать владельцу причину "
            "(владелец 10-14: SCR-DETAILS 5 дней в бэклоге после SCR-PORT-2).")


def main():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        sys.exit(0)
    event = data.get("hook_event_name", "")
    tool = data.get("tool_name") or ""
    tin = data.get("tool_input") or {}
    s = load()

    if event == "SubagentStop":
        code = s["agents"].get(data.get("agent_id") or "") or \
            code_from_transcript(data.get("agent_transcript_path") or "")
        if code:
            s["pending"][code] = time.time()
            save(s)
        sys.exit(0)

    if data.get("agent_id"):
        sys.exit(0)  # a sub-agent's own calls: the board is the lead's job

    if event == "Stop":
        late = unaccepted(s)
        if late and not data.get("stop_hook_active"):
            emit({"decision": "block", "reason": "[board_guard] Агенты сдали, карточки не обновлены: " + ", ".join(late) +
                  ". Приём = карточка: Статус (На проверке/Сделано/В работе), «Кто ведёт», итог и остаток в теле."})
        if s["unblock"] and not data.get("stop_hook_active"):
            emit({"decision": "block", "reason": "[board_guard] " + unblock_reason(sorted(s["unblock"]))})
        sys.exit(0)

    if event == "PostToolUse" and tool.startswith("mcp__notion__"):
        resp = "\n".join(strings(data.get("tool_response")))
        for pid, code in TITLE_RE.findall(resp) + CREATED_RE.findall(resp):
            s["pages"][norm(pid)] = code
        now = time.time()
        if tool.endswith("notion-create-pages"):
            for page in tin.get("pages") or []:
                code = (page.get("properties") or {}).get("Код")
                if code:
                    s["writes"][code] = now
        elif tool.endswith("notion-update-page"):
            code = s["pages"].get(norm(tin.get("page_id")))
            if code:
                s["writes"][code] = now
                status = (tin.get("properties") or {}).get("Статус")
                if status in ("На проверке", "Сделано"):
                    s["unblock"][code] = now
        elif tool.endswith("notion-query-data-sources"):
            sql = json.dumps(tin, ensure_ascii=False)
            if "Владелец назвал" in sql:
                for code in [c for c in s["unblock"] if re.search(r"(?<![A-Z0-9-])" + re.escape(c) + r"(?![A-Z0-9])", sql)]:
                    del s["unblock"][code]
        save(s)
        sys.exit(0)

    if event == "PostToolUse" and tool == "Agent":
        code = code_from_text(str(tin.get("prompt") or ""))
        m = re.search(r"agentId\"?\s*[:=]\s*\"?([0-9a-z]{8,})", json.dumps(data.get("tool_response"), ensure_ascii=False))
        if code and m:
            s["agents"][m.group(1)] = code
            save(s)
        sys.exit(0)

    if event == "PostToolUse" and tool in ("Bash", "Edit", "Write"):
        text = str(tin.get("command") or tin.get("file_path") or "")
        writes_remainder = re.search(r">>?\s*\S*REMAINDER\.md", text) if tool == "Bash" else text.endswith("REMAINDER.md")
        if writes_remainder:
            emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext":
                  "[board_guard] Решение тимлида в REMAINDER — продублируй В ТЕЛО КАРТОЧКИ задачи (владелец 10-09); "
                  "REMAINDER агенту, карточка владельцу."}})
        sys.exit(0)

    if event == "PreToolUse" and tool in ("Agent", "SendMessage"):
        if tool == "Agent" and (tin.get("subagent_type") or "").lower() == "explore":
            sys.exit(0)
        late = unaccepted(s)
        if late:
            deny("сначала приём на доске: агенты сдали, карточки не обновлены — " + ", ".join(late) +
                 ". notion-update-page: Статус, «Кто ведёт», итог/остаток в теле.")
        if tool == "Agent" and s["unblock"]:
            deny(unblock_reason(sorted(s["unblock"])))
        if tool == "Agent":
            code = code_from_text(str(tin.get("prompt") or ""))
            if not code:
                deny("в промпте нет кода задачи («You are agent <КОД>» или /private/tmp/claude-501/<КОД>/BRIEF.md) — "
                     "без кода карточку не сверить.")
            if code not in s["writes"]:
                deny(f"нет карточки {code} на доске «Задачи»: создай/обнови ДО запуска (Код={code}, Статус «В работе», "
                     "Ветка, «Кто ведёт»; заголовок «КОД · …»). Владелец 10-09: «почему я каждый раз тебя тыкаю?»")
    sys.exit(0)


if __name__ == "__main__":
    main()
