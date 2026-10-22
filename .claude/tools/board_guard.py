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
   notion-query-data-sources whose SQL names «Владелец назвал» is run, agent launches are refused: every owner-named
   card waiting on a handed-in code is either launched or reported to the owner. One query clears every pending code;
   the turn may end meanwhile (owner 10-19: blocking Stop re-ran the lead's whole turn after each hand-in).

State: ~/.claude/board_guard_state.json {pages: {page_id: code}, writes: {code: t}, pending: {code: t}, agents: {id: code}}.
"""
import json
import os
import re
import subprocess
import sys
import time

# Windows: the hook event and our messages are UTF-8, not the console code page (cp1251 lost «Код» on 10-20).
for _stream in (sys.stdin, sys.stdout, sys.stderr):
    _stream.reconfigure(encoding="utf-8")

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
    for k in ("pages", "writes", "pending", "agents", "unblock", "launched", "stopped"):
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


RUNNING_MAX_AGE = 4 * 3600  # an agent with no SubagentStop older than this died with its session (crash, reboot)


def running_agent(s, code):
    """The id of a launched agent for @p code that has not stopped yet, or None."""
    now = time.time()
    for aid, c in s["agents"].items():
        if c == code and aid not in s["stopped"] and now - s["launched"].get(aid, 0) < RUNNING_MAX_AGE:
            return aid
    return None


def running_count(s):
    now = time.time()
    return sum(1 for aid in s["agents"] if aid not in s["stopped"] and now - s["launched"].get(aid, 0) < RUNNING_MAX_AGE)


# Night mode (owner 10-22 «ночью управляй сам агентами, без простоя»): ~/.claude/night_mode holds an epoch
# end time; `night_mode.sh on [HH:MM]` / `off` write it. While it is in the future, Stop refuses an idle lead.
NIGHT_FILE = os.path.expanduser("~/.claude/night_mode")
NIGHT_MIN_AGENTS = 3


def night_until():
    try:
        with open(NIGHT_FILE) as f:
            until = float(f.read().strip())
    except (OSError, ValueError):
        return 0
    return until if until > time.time() else 0


def landing():
    """A merge (land_dev.sh) running counts as the working slot it is."""
    return subprocess.run(["pgrep", "-f", "land_dev.sh"], capture_output=True).returncode == 0


def unaccepted(s):
    return sorted(c for c, t in s["pending"].items() if s["writes"].get(c, 0) < t)


UNBLOCK_SQL = ("SELECT \"Код\",\"Статус\",\"Название\" FROM \"collection://386f5e0e-e749-479b-83a7-59e03ef9b8b2\" "
               "WHERE \"Приоритет\"='Владелец назвал' AND \"Статус\" IN ('Бэклог','Готово к старту','Заблокировано') "
               "-- после {codes}")


def unblock_reason(codes):
    return ("сдано " + ", ".join(codes) + " — проверь, что ждало этого среди карточек «Владелец назвал»: "
            "один notion-query-data-sources с «Владелец назвал» на все сданные коды, напр. " + UNBLOCK_SQL.format(codes=" ".join(codes)) +
            " (затем открой карточки, где в теле «после <КОД>»). Ждавшее — запустить сразу или назвать владельцу причину "
            "(владелец 10-14: SCR-DETAILS 5 дней в бэклоге после SCR-PORT-2).")


PUSH_RE = re.compile(r"\bgit\b[^;&|]*\bpush\b([^;&|]*)")
STALE_SECONDS = 24 * 3600
MAX_BEHIND = 40


def push_refusal(cmd):
    """Trunk-based push rule (owner 10-22): refuse creating int/*; refuse a task/* branch older than a day or far behind dev."""
    m = PUSH_RE.search(cmd)
    if not m:
        return None
    args = m.group(1)
    if re.search(r"(?:^|[\s:])int/", args):
        return ("[board_guard] int/* больше не создаём (trunk-based, владелец 10-22): принятая ветка вливается в dev "
                "сама (land_dev.sh); остаток бисекта — только по слову тимлида.")
    task = re.search(r"(?:^|[\s:])(task/[\w.\-]+)", args)
    if not task:
        return None
    cwd = re.search(r"\bgit\s+-C\s+(\S+)", cmd)
    git = ["git"] + (["-C", cwd.group(1).strip("'\"")] if cwd else [])
    try:
        base = subprocess.run(git + ["merge-base", "origin/dev", "HEAD"], capture_output=True, text=True, timeout=10).stdout.strip()
        if not base:
            return None
        born = int(subprocess.run(git + ["log", "-1", "--format=%ct", base], capture_output=True, text=True, timeout=10).stdout.strip() or 0)
        behind = int(subprocess.run(git + ["rev-list", "--count", base + "..origin/dev"], capture_output=True, text=True, timeout=10).stdout.strip() or 0)
    except (OSError, ValueError, subprocess.SubprocessError):
        return None
    if born and time.time() - born > STALE_SECONDS and behind > MAX_BEHIND:
        return (f"[board_guard] {task.group(1)} отрезана от dev {int((time.time() - born) / 3600)} ч назад и отстала на {behind} "
                f"коммитов (trunk-based: ветка ≤1 дня). Сначала `git rebase origin/dev` (или влить), потом push.")
    return None


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
        s["stopped"][data.get("agent_id") or ""] = time.time()
        if code:
            s["pending"][code] = time.time()
        save(s)
        sys.exit(0)

    if event == "PreToolUse" and tool == "Bash":
        # Trunk-based (owner 10-22): no new int/* branches; a task/* push that is stale or far behind dev is warned.
        reason = push_refusal(str(tin.get("command") or ""))
        if reason:
            deny(reason)
        sys.exit(0)

    if data.get("agent_id"):
        sys.exit(0)  # a sub-agent's own calls: the board is the lead's job

    if event == "PreToolUse" and tool.endswith("notion-update-page"):
        # Owner 10-22: a card reaches «Сделано» only once its commit is named and is in dev.
        props = tin.get("properties") or {}
        if props.get("Статус") == "Сделано" and (not str(props.get("Коммит") or "").strip() or props.get("В dev") != "__YES__"):
            deny("[board_guard] «Сделано» только с «Коммит» (SHA) и «В dev» = __YES__ в том же вызове (trunk-based, владелец 10-22). "
                 "Не в dev — «На проверке».")
        sys.exit(0)

    if event == "Stop":
        late = unaccepted(s)
        if late and not data.get("stop_hook_active"):
            emit({"decision": "block", "reason": "[board_guard] Агенты сдали, карточки не обновлены: " + ", ".join(late) +
                  ". Приём = карточка: Статус (На проверке/Сделано/В работе), «Кто ведёт», итог и остаток в теле."})
        # Rule 4 is NOT enforced at Stop (owner 10-19: a blocked Stop re-ran the whole lead turn after every
        # hand-in); it is enforced before the next Agent launch, where the check actually changes what runs.
        night = night_until()
        if night and not data.get("stop_hook_active"):
            busy = running_count(s)
            if busy < NIGHT_MIN_AGENTS and not landing():
                emit({"decision": "block", "reason": "[board_guard] Ночной режим до " +
                      time.strftime("%H:%M", time.localtime(night)) + f" (владелец спит): работает агентов {busy} < "
                      f"{NIGHT_MIN_AGENTS}, слияния нет. Запусти следующую карточку по хвосту HANDOVER — простой = промах."})
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
                # One query over the owner-named backlog answers for every code handed in so far (owner 10-19).
                s["unblock"].clear()
        save(s)
        sys.exit(0)

    if event == "PostToolUse" and tool == "Agent":
        code = code_from_text(str(tin.get("prompt") or ""))
        m = re.search(r"agentId\"?\s*[:=]\s*\"?([0-9a-z]{8,})", json.dumps(data.get("tool_response"), ensure_ascii=False))
        if code and m:
            s["agents"][m.group(1)] = code
            s["launched"][m.group(1)] = time.time()
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
                 ". accept.sh, затем по ~/.claude/tools/lead_templates.md §6: update_properties (Статус, «Чем доказано», Токены) + insert_content (итог/остаток).")
        if tool == "Agent" and s["unblock"]:
            deny(unblock_reason(sorted(s["unblock"])))
        if tool == "Agent":
            code = code_from_text(str(tin.get("prompt") or ""))
            if not code:
                deny("в промпте нет кода задачи («You are agent <КОД>» или /private/tmp/claude-501/<КОД>/BRIEF.md) — "
                     "без кода карточку не сверить. Промпт — шаблон ~/.claude/tools/lead_templates.md §5.")
            alive = running_agent(s, code)
            if alive:
                deny(f"агент {code} ещё работает ({alive}): второй агент на ту же задачу правит то же дерево и сжигает "
                     "токены впустую (10-20: двойной PT-GLASS, 2,3 млн). Дождись его уведомления или TaskStop, потом запускай.")
            if code not in s["writes"]:
                deny(f"нет карточки {code} на доске «Задачи»: создай/обнови ДО запуска (Код={code}, Статус «В работе», "
                     "Ветка, «Кто ведёт»; заголовок «КОД · …»): notion-search по коду → update_properties по шаблону "
                     "~/.claude/tools/lead_templates.md §6, без fetch схемы. Владелец 10-09: «почему я каждый раз тебя тыкаю?»")
    sys.exit(0)


if __name__ == "__main__":
    main()
