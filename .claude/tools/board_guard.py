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

5. Tails (owner 10-27: «я несколько недель напоминаю про хвосты, а ты всё равно забываешь после clear»). The board is
   mirrored into state["board"] from every query/update/create. A tail = a card of this machine in В работе / Готово к
   старту / На проверке / Заблокировано. SessionStart prints the tails (--session-start); a launch of anything that is not
   a tail is refused while tails exist, unless the prompt carries «ХВОСТ: <tail code>» (a closer for that tail) or
   «ВЛАДЕЛЕЦ: «<quote>»» with a quote the owner typed this session; Stop is blocked once while a card of this machine is
   «В работе» with no live agent (abandoned work — set its honest status or relaunch it).

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
        with open(STATE, encoding="utf-8") as f:
            s = json.load(f)
    except (OSError, ValueError):
        s = {}
    for k in ("pages", "writes", "pending", "agents", "unblock", "launched", "stopped", "board"):
        s.setdefault(k, {})
    s.setdefault("prompts", [])
    s.setdefault("board_t", 0)
    return s


def save(s):
    tmp = "%s.%d.tmp" % (STATE, os.getpid())  # per-process: two hooks sharing one tmp interleaved their writes
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(s, f)  # ASCII: open() uses the locale code page, which has no «→» / «≤» from board rows
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
# Day floor (owner 10-29 «у тебя опять 2 агента. я разрешил больше ... отслеживай через хуки»): ~/.claude/min_agents
# holds the number of agents that must be running; Stop refuses a lead below it unless a merge is in progress.
MIN_AGENTS_FILE = os.path.expanduser("~/.claude/min_agents")
MAX_AGENTS = 4  # owner 10-30 «не меньше 2, не больше 4»: a launch above it is refused


def min_agents():
    try:
        with open(MIN_AGENTS_FILE) as f:
            return int(f.read().strip() or 0)
    except (OSError, ValueError):
        return 0


def night_until():
    try:
        with open(NIGHT_FILE) as f:
            until = float(f.read().strip())
    except (OSError, ValueError):
        return 0
    return until if until > time.time() else 0


def landing():
    """A merge (land_dev.sh) running counts as the working slot it is."""
    try:
        return subprocess.run(["pgrep", "-f", "land_dev.sh"], capture_output=True).returncode == 0
    except OSError:
        pass
    # Windows has no pgrep: the command lines of the running processes, through CIM
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command",
                              "(Get-CimInstance Win32_Process -Filter \"CommandLine like '%land_dev.sh%' "
                              "and not CommandLine like '%Win32_Process%'\").Count"],
                             capture_output=True, text=True, timeout=20).stdout.strip()
        return out.isdigit() and int(out) > 0
    except (OSError, subprocess.TimeoutExpired):
        return False


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


# ---- Tails (rule 5) --------------------------------------------------------------------------------------------------
BOARD = "collection://386f5e0e-e749-479b-83a7-59e03ef9b8b2"
TAIL_STATUSES = ("В работе", "Готово к старту", "На проверке", "Заблокировано")
TAILS_SQL = ("SELECT url,\"Код\",\"Статус\",\"Зона\",\"Приоритет\",substr(\"Кто ведёт\",1,60) AS who,substr(\"Обновлено\",1,10) AS upd "
             f"FROM \"{BOARD}\" WHERE \"Статус\" IN ('В работе','Готово к старту','На проверке','Заблокировано') -- TAILS")
FOREIGN = "Mac" if sys.platform == "win32" else "Windows"


def foreign(row):
    who = (row.get("who") or "").lstrip()
    return who.startswith("Mac") if FOREIGN == "Mac" else who.startswith("Windows")


def tails(s):
    return {c: r for c, r in s["board"].items() if r.get("status") in TAIL_STATUSES and not foreign(r)}


def abandoned(s):
    if landing():
        return []
    return sorted(c for c, r in tails(s).items() if r.get("status") == "В работе" and not running_agent(s, c))


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
            if row.get("url"):  # a later update-page by page id mirrors onto this code (VFX-12 went unmirrored, 10-30)
                s["pages"][norm(row["url"])] = code
            cur = s["board"].setdefault(code, {})
            cur["status"] = status
            for src, dst in (("Зона", "zone"), ("Приоритет", "prio"), ("who", "who"), ("Кто ведёт", "who"), ("upd", "upd")):
                if row.get(src) is not None:
                    cur[dst] = str(row[src])[:60]
    if full and not partial:  # a paged answer (has_more) has not seen every tail: closing the unseen would lie
        for code in list(s["board"]):
            if code not in seen and s["board"][code].get("status") in TAIL_STATUSES:
                s["board"][code]["status"] = "закрыта"
    if full:
        s["board_t"] = time.time()


def mirror_fresh(s):
    """Owner 10-30: the board is read from Notion once per session (after /clear or /compact, and before a handover);
    in between, the lead's own writes keep the local mirror current (create/update-page are mirrored above)."""
    return s["board_t"] > 0 and s["board_t"] >= s.get("session_t", 0)


def owner_named_waiting(s):
    return sorted((c, r.get("status")) for c, r in s["board"].items() if r.get("prio") == "Владелец назвал"
                  and r.get("status") in ("Бэклог", "Готово к старту", "Заблокировано"))


def owner_quote_ok(s, prompt):
    m = re.search(r"ВЛАДЕЛЕЦ:\s*«([^»]{3,})»", prompt)
    if not m:
        return False
    q = m.group(1).strip().lower()
    # a short consent («Запускай») counts only as the owner's whole message, never as a substring of one
    return any((q in t.lower()) if len(q) >= 12 else (q == t.strip().strip(".!").lower()) for _, t in s["prompts"])


def launch_refusal(s, code, prompt):
    """Rule 5: while tails exist, only a tail (or a closer naming one, or an owner-quoted feature) may be launched."""
    if not s["board_t"]:
        return ("зеркала доски нет — сначала один notion-query-data-sources с хвостами (он заполнит зеркало): " + TAILS_SQL)
    t = tails(s)
    row = s["board"].get(code) or {}
    if not t or (row.get("status") in TAIL_STATUSES and not row.get("new")) or code in s["agents"].values():
        return None  # a tail, or a later pass of work already started
    named = re.search(r"ХВОСТ:\s*(" + CODE_RE + r")", prompt)
    if named and named.group(1) in t:
        return None
    if owner_quote_ok(s, prompt):
        return None
    return (f"{code} — не хвост (статус «{row.get('status', 'Бэклог/новая')}»), а открытых хвостов {len(t)} "
            "(владелец 10-27: хвосты первыми, «фичи можно, если хвосты закрыты»). Запусти хвост; закрывашку хвоста помечай "
            "в промпте «ХВОСТ: <КОД хвоста>»; новую фичу — только с «ВЛАДЕЛЕЦ: «<его слова этой сессии>»».")


# Owner 10-29 «почему так много неслитых веток??»: 16 branches, 7 of them ~340 h, because new steps went into
# branches nobody landed. A launch on a task branch whose oldest commit not in dev is older than a day is refused
# unless the prompt marks it as landing work («ПОСАДКА:» — merge, build-readiness, compile fixes; no new feature).
def stale_branch_refusal(code, prompt):
    if "ПОСАДКА:" in prompt:
        return None
    base = re.sub(r"-\d+$", "", code)
    for br in (f"origin/task/{code}", f"origin/task/{base}"):
        r = subprocess.run(["git", "log", "--format=%ct", "--reverse", f"origin/dev..{br}"],
                           capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            age = (time.time() - int(r.stdout.split()[0])) / 3600
            if age > 24:
                return (f"{br} не в dev уже {age:.0f} ч: новый шаг в невлитую ветку запрещён (владелец 10-29, «почему так "
                        "много неслитых веток??»). Сначала влей её; работа на посадку (слияние, готовность к сборке, "
                        "ошибки компиляции) — с «ПОСАДКА: <что>» в промпте.")
            return None
    return None


def session_start(s):
    t = tails(s)
    s["session_t"] = time.time()
    save(s)
    out = ["[tails] зеркало доски обновляется раз за сессию (владелец 10-30) — ПЕРВЫЙ вызов сессии: "
           "notion-query-data-sources " + TAILS_SQL + " ; перед передачей/паузой — тот же запрос ещё раз."]
    if t:
        by = {}
        for c, r in t.items():
            by.setdefault(r.get("status"), []).append(c)
        owner = sorted((c for c, r in t.items() if r.get("prio") == "Владелец назвал"), key=lambda c: t[c].get("upd") or "")
        out.append(f"[tails] ХВОСТЫ ПЕРВЫМИ (владелец 10-27, после /clear не забывать): открыто {len(t)} — " +
                   "; ".join(f"{k} {len(v)}" for k, v in sorted(by.items())))
        gone = abandoned(s)
        if gone:
            out.append("[tails] БРОШЕНО («В работе» без агента): " + " ".join(gone))
        if owner:
            out.append(f"[tails] «Владелец назвал» {len(owner)}, старейшие: " + " ".join(owner[:20]) + (" …" if len(owner) > 20 else ""))
        out.append("[tails] Новая фича не запустится, пока хвосты открыты (board_guard), кроме «ВЛАДЕЛЕЦ: «цитата»».")
    print("\n".join(out))


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
    # the repo the push runs in: `git -C <dir>`, else the last `cd <dir>` before the push (10-26: a `cd F:/wt/X; git
    # push` was measured against the main checkout's HEAD and an up-to-date branch was refused as 49 h stale)
    cwd = re.search(r"\bgit\s+-C\s+(\S+)", cmd)
    if not cwd:
        cds = re.findall(r"(?:^|[;&|]\s*)cd\s+(\"[^\"]+\"|'[^']+'|\S+)", cmd[:m.start()])
        cwd = re.match(r"(.*)", cds[-1]) if cds else None
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
    if "--session-start" in sys.argv:
        session_start(load())
        sys.exit(0)
    if "--owner-named-mirror" in sys.argv:
        # Notion's SQL allowance runs out (10-27): the unblock check is answered from the board mirror while it is fresh.
        s = load()
        if not mirror_fresh(s):
            print("зеркало доски не обновлено в этой сессии — нужен настоящий запрос (TAILS_SQL)")
            sys.exit(1)
        rows = owner_named_waiting(s)
        print(f"после {' '.join(sorted(s['unblock']))} ждут «Владелец назвал» ({len(rows)}): " +
              " ".join(f"{c}[{st}]" for c, st in rows))
        s["unblock"].clear()
        save(s)
        sys.exit(0)
    try:
        data = json.load(sys.stdin)
    except ValueError:
        sys.exit(0)
    event = data.get("hook_event_name", "")
    tool = data.get("tool_name") or ""
    tin = data.get("tool_input") or {}
    s = load()

    if event == "UserPromptSubmit":
        text = str(data.get("prompt") or "")
        if text.lstrip().startswith(("<task-notification>", "<system-reminder>", "<cross-session-message")):
            sys.exit(0)  # a background notice, not the owner's words: it must not crowd the owner's quotes out
        s["prompts"] = [p for p in s["prompts"] if time.time() - p[0] < 48 * 3600][-40:] + \
            [[time.time(), str(data.get("prompt") or "")[:4000]]]
        save(s)
        sys.exit(0)

    if event == "SubagentStop":
        # A worker's nested child (an Explore it spawned) stops with the worker's agent_id; its own id is only in the
        # transcript name, so a live FPS-MEASURE-5 read as stopped (10-30) - the transcript name wins.
        named = re.search(r"agent-([0-9a-f]{8,})\.jsonl$", str(data.get("agent_transcript_path") or ""))
        aid = named.group(1) if named else (data.get("agent_id") or "")
        if aid not in s["agents"] and (data.get("agent_id") or "") in s["agents"]:
            sys.exit(0)  # the worker's own child: nothing was handed in to the lead
        code = s["agents"].get(aid) or code_from_transcript(data.get("agent_transcript_path") or "")
        s["stopped"][aid] = time.time()
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
        gone = abandoned(s)
        if gone and not late and not data.get("stop_hook_active"):
            emit({"decision": "block", "reason": "[board_guard] Брошено — «В работе» без живого агента: " + " ".join(gone) +
                  ". Запусти или поставь честный статус (Готово к старту / Заблокировано с причиной) — владелец 10-27: хвосты не бросать."})
        floor = min_agents()
        if floor and not night_until() and not late and not data.get("stop_hook_active"):
            busy = running_count(s)
            if busy < floor and not landing():
                emit({"decision": "block", "reason": f"[board_guard] Работает агентов {busy} < {floor} (владелец 10-29: минимум 3, "
                      "максимум 5, ~/.claude/min_agents). Запусти следующие карточки из очереди HANDOVER, потом заканчивай ход."})
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
                props = page.get("properties") or {}
                code = props.get("Код")
                if code:
                    s["writes"][code] = now
                    s["board"][code] = {"status": props.get("Статус") or "Бэклог", "zone": props.get("Зона"),
                                        "prio": props.get("Приоритет"), "who": str(props.get("Кто ведёт") or "")[:60], "new": now}
        elif tool.endswith("notion-update-page"):
            renamed = (tin.get("properties") or {}).get("Код")
            if renamed:  # a card re-coded by update_properties is that code from now on (BISTRO-MEM → -2, 10-28)
                s["pages"][norm(tin.get("page_id"))] = renamed
            code = s["pages"].get(norm(tin.get("page_id")))
            if code:
                s["writes"][code] = now
                props = tin.get("properties") or {}
                status = props.get("Статус")
                row = s["board"].setdefault(code, {})
                if status:
                    row["status"] = status
                if props.get("Кто ведёт"):
                    row["who"] = str(props["Кто ведёт"])[:60]
                if status in ("На проверке", "Сделано"):
                    s["unblock"][code] = now
        elif tool.endswith("notion-query-data-sources"):
            sql = json.dumps(tin, ensure_ascii=False)
            ingest_rows(s, list(strings(data.get("tool_response"))), "-- TAILS" in sql, "OFFSET" in sql.upper())
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
            if not s["board_t"]:
                deny(unblock_reason(sorted(s["unblock"])))
            # Answered from the mirror, no Notion query (owner 10-30): the waiting owner-named cards are shown once.
            done = sorted(s["unblock"])
            rows = owner_named_waiting(s)
            s["unblock"].clear()
            save(s)
            sys.stderr.write(f"[board_guard] после {' '.join(done)} ждут «Владелец назвал» ({len(rows)}): " +
                             " ".join(f"{c}[{st}]" for c, st in rows) + " — открой те, где в теле «после <КОД>»." +
                             ("" if mirror_fresh(s) else " ЗЕРКАЛО СТАРОЕ (запрос этой сессии не прошёл, напр. лимит Notion): "
                              "повтори TAILS-запрос при следующем приёме.") + "\n")
        if tool == "Agent":
            code = code_from_text(str(tin.get("prompt") or ""))
            if not code:
                deny("в промпте нет кода задачи («You are agent <КОД>» или /private/tmp/claude-501/<КОД>/BRIEF.md) — "
                     "без кода карточку не сверить. Промпт — шаблон ~/.claude/tools/lead_templates.md §5.")
            busy = running_count(s)
            if busy >= MAX_AGENTS:
                deny(f"работает агентов {busy}, максимум {MAX_AGENTS} (владелец 10-29 «минимум 3, а максимум 5»). "
                     "Дождись сдачи одного, прими его, потом запускай.")
            alive = running_agent(s, code)
            if alive:
                deny(f"агент {code} ещё работает ({alive}): второй агент на ту же задачу правит то же дерево и сжигает "
                     "токены впустую (10-20: двойной PT-GLASS, 2,3 млн). Дождись его уведомления или TaskStop, потом запускай.")
            refusal = code not in s["writes"] or launch_refusal(s, code, str(tin.get("prompt") or ""))
            if isinstance(refusal, str):
                deny(refusal)
            stale = stale_branch_refusal(code, str(tin.get("prompt") or ""))
            if stale:
                deny(stale)
            if code not in s["writes"]:
                deny(f"нет карточки {code} на доске «Задачи»: создай/обнови ДО запуска (Код={code}, Статус «В работе», "
                     "Ветка, «Кто ведёт»; заголовок «КОД · …»): notion-search по коду → update_properties по шаблону "
                     "~/.claude/tools/lead_templates.md §6, без fetch схемы. Владелец 10-09: «почему я каждый раз тебя тыкаю?»")
    sys.exit(0)


if __name__ == "__main__":
    main()
