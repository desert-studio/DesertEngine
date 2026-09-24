#!/usr/bin/env python3
"""PreToolUse / PostToolUse guard that ENFORCES the agents' context budget (contract §7, brief «Контекст»).

The rules were advice for a day and no agent followed them (measured 2026-09-24: zero Explore calls, shell
search 25-38 % of every task, 95-180-turn tasks, cache rewrites of up to 566k tokens after long waits).
The owner: «Заставь агентов выполнять это». So the rules are checked on every tool call of a SUB-AGENT:

  1. no tree-wide search in a worker's own context (grep -r, rg, git grep, find without -maxdepth) —
     discovery goes to an Explore sub-agent, whose reading stays in its own context;
  2. at most 80 tool calls per worker — from call 60 every result carries a stop-at-a-step-boundary reminder,
     past 80 everything is refused except git, SendMessage and reading/writing the report;
  3. no `sleep` longer than 270 s in one call (the prompt cache expires at 5 minutes);
  4. at most two editor builds per worker;
  5. one `make` on the machine at a time, at most -j4 (16 GB RAM; 2026-09-24 parallel builds OOM-killed it);
  6. one editor/runtime process (GPU) at a time (2026-09-24 concurrent editors hung WindowServer: kernel panic).

The lead's own session (no agent id in the hook input) and Explore agents are never restricted.
Every decision is appended to /tmp/claude-501/agent-guard/log.jsonl so the effect can be measured.
"""
import json
import os
import re
import subprocess
import sys
import time

STATE_DIR = "/tmp/claude-501/agent-guard"
TURN_WARN = 60
TURN_LIMIT = 80
MAX_SLEEP = 270
MAX_EDITOR_BUILDS = 2

TREE_SEARCH = [
    re.compile(r"(^|[;&|(]\s*|\s)(grep|egrep|fgrep)\s+(-[A-Za-z]*[rR][A-Za-z]*|--recursive)\b"),
    re.compile(r"(^|[;&|(]\s*|\s)rg\s"),
    re.compile(r"(^|[;&|(]\s*|\s)git\s+grep\b"),
    re.compile(r"(^|[;&|(]\s*|\s)(ag|ack)\s"),
]
FIND = re.compile(r"(^|[;&|(]\s*|\s)find\s")
SLEEP = re.compile(r"\bsleep\s+(\d+)")
EDITOR_BUILD = re.compile(r"\bmake\b[^;&|]*\bEditor\b")
MAKE = re.compile(r"(^|[;&|(]\s*|\s)make\s")
MAKE_JOBS = re.compile(r"\bmake\b[^;&|]*?-j\s*(\d+)")
MAX_MAKE_JOBS = 4
SELF_WAIT = re.compile(r"pgrep\s+-x\s+make")  # a command that waits for the other build itself is allowed
EDITOR_RUN = re.compile(r"Bin/(Debug|Release)/(Editor|Runtime)\b")
ALWAYS_ALLOWED_AFTER_LIMIT =re.compile(r"^\s*(cd [^;&]+&&\s*)?git\s")


def emit(obj):
    sys.stdout.write(json.dumps(obj))
    sys.exit(0)


def deny(reason, data, agent):
    log(data, agent, "deny", reason)
    emit({"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny",
                                 "permissionDecisionReason": reason}})


def log(data, agent, decision, note=""):
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(os.path.join(STATE_DIR, "log.jsonl"), "a") as f:
            f.write(json.dumps({"t": time.time(), "agent": agent, "type": data.get("agent_type"),
                                "tool": data.get("tool_name"), "decision": decision, "note": note[:200]}) + "\n")
    except OSError:
        pass


def load_state(agent):
    path = os.path.join(STATE_DIR, f"{agent}.json")
    try:
        with open(path) as f:
            return json.load(f), path
    except (OSError, ValueError):
        return {"calls": 0, "editor_builds": 0}, path


def save_state(state, path):
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(path, "w") as f:
            json.dump(state, f)
    except OSError:
        pass


def other_build_running():
    """One build on the machine at a time (16 GB: three parallel builds OOM-killed it on 2026-09-24)."""
    try:
        return subprocess.run(["pgrep", "-x", "make"], capture_output=True).returncode == 0
    except OSError:
        return False


def editor_running():
    """One editor/runtime (GPU) at a time: concurrent MoltenVK editors hung WindowServer and the watchdog
    panicked the kernel on 2026-09-24 08:05."""
    try:
        return subprocess.run(["pgrep", "-f", "Bin/(Debug|Release)/(Editor|Runtime)"],
                              capture_output=True).returncode == 0
    except OSError:
        return False


def main():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        sys.exit(0)

    # Verified 2026-09-24 on live calls: a sub-agent's input carries agent_id and agent_type.
    agent = data.get("agent_id")
    agent_type = (data.get("agent_type") or "").lower()
    if not agent or agent_type == "explore":
        sys.exit(0)  # the lead's own session, or a discovery agent: unrestricted

    event = data.get("hook_event_name", "PreToolUse")
    state, path = load_state(agent)

    if event == "PostToolUse":
        calls = state.get("calls", 0)
        if calls >= TURN_WARN:
            emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext":
                  f"[agent_guard] Вызов {calls}/{TURN_LIMIT}. Остановись на границе шага: коммит, пуш, отчёт "
                  f"тимлиду. После {TURN_LIMIT} разрешены только git, SendMessage и отчёт."}})
        sys.exit(0)

    tool = data.get("tool_name", "")
    tin = data.get("tool_input") or {}
    cmd = tin.get("command", "") if tool == "Bash" else ""

    state["calls"] = state.get("calls", 0) + 1
    calls = state["calls"]

    if calls > TURN_LIMIT:
        allowed = tool in ("SendMessage", "Write") or (tool == "Bash" and ALWAYS_ALLOWED_AFTER_LIMIT.match(cmd))
        if not allowed:
            save_state(state, path)
            deny(f"[agent_guard] Лимит {TURN_LIMIT} вызовов исчерпан ({calls}). Закоммить и запушь сделанное, "
                 f"отчитайся тимлиду; остаток он отдаст свежему агенту.", data, agent)

    if tool == "Bash":
        for rx in TREE_SEARCH:
            if rx.search(cmd):
                save_state(state, path)
                deny("[agent_guard] Поиск по дереву в своём контексте запрещён (контракт §7.3). Отдай вопрос "
                     "субагенту: Agent(subagent_type: \"Explore\", prompt: \"<что найти, ответ ≤60 строк>\"). "
                     "Сам ищи только внутри уже известных файлов: grep -n <шаблон> <файл>.", data, agent)
        if FIND.search(cmd) and "-maxdepth" not in cmd:
            save_state(state, path)
            deny("[agent_guard] `find` без -maxdepth — это поиск по дереву; используй Explore или "
                 "`find <папка> -maxdepth 2 ...`.", data, agent)
        for m in SLEEP.finditer(cmd):
            if int(m.group(1)) > MAX_SLEEP:
                save_state(state, path)
                deny(f"[agent_guard] sleep {m.group(1)} > {MAX_SLEEP} с: кэш истекает через 5 минут и весь "
                     f"контекст пишется заново. Жди кусками: for i in $(seq 27); do grep -q <маркер> <лог> && "
                     f"break; sleep 10; done", data, agent)
        if MAKE.search(cmd):
            jobs = [int(j) for j in MAKE_JOBS.findall(cmd)]
            if any(j > MAX_MAKE_JOBS for j in jobs):
                save_state(state, path)
                deny(f"[agent_guard] make -j{max(jobs)} > -j{MAX_MAKE_JOBS}: 16 ГБ памяти, clang этого движка "
                     f"берёт 1-2 ГБ на файл. Собирай с -j{MAX_MAKE_JOBS}.", data, agent)
            if other_build_running() and not SELF_WAIT.search(cmd):
                save_state(state, path)
                deny("[agent_guard] На машине уже идёт сборка (другой агент или тимлид). Одновременно — только "
                     "одна: 2026-09-24 три параллельные сборки съели 16 ГБ и уронили машину. Поставь ожидание перед "
                     "make в ту же команду: for i in $(seq 27); do pgrep -x make >/dev/null || break; sleep 10; "
                     "done; make ...",
                     data, agent)
        if EDITOR_RUN.search(cmd) and not re.search(r"\bpkill\b|\bpgrep\b", cmd) and editor_running():
            save_state(state, path)
            deny("[agent_guard] Уже запущен редактор/рантайм (другой агент). Одновременно — только ОДИН процесс с GPU: "
                 "2026-09-24 несколько редакторов повесили WindowServer, ядро ушло в panic. Подожди в той же команде: "
                 "for i in $(seq 27); do pgrep -f 'Bin/(Debug|Release)/(Editor|Runtime)' >/dev/null || break; "
                 "sleep 10; done; <запуск>", data, agent)
        if EDITOR_BUILD.search(cmd):
            if state.get("editor_builds", 0) >= MAX_EDITOR_BUILDS:
                save_state(state, path)
                deny(f"[agent_guard] Editor уже собирался {MAX_EDITOR_BUILDS} раза в этой задаче. Итерации — на "
                     f"сюитах; если без третьей сборки нельзя — SendMessage тимлиду с причиной.", data, agent)
            state["editor_builds"] = state.get("editor_builds", 0) + 1

    save_state(state, path)
    log(data, agent, "allow")
    sys.exit(0)


if __name__ == "__main__":
    main()
