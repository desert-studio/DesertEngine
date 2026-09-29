#!/usr/bin/env python3
"""PreToolUse / PostToolUse guard that ENFORCES the agents' context budget (contract §7, brief «Контекст»).

The rules were advice for a day and no agent followed them (measured 2026-09-24: zero Explore calls, shell
search 25-38 % of every task, 95-180-turn tasks, cache rewrites of up to 566k tokens after long waits).
The owner: «Заставь агентов выполнять это». So the rules are checked on every tool call of a SUB-AGENT:

  1. no tree-wide search in a worker's own context (grep -r, rg, git grep, find without -maxdepth) —
     discovery goes to an Explore sub-agent, whose reading stays in its own context;
  2. at most 60 tool calls per worker, a HARD cap no extension lifts (ledger 09-29: a call costs 8k at <=30 calls and
     19k past 90; agents past 90 were 45 % of all spend) — from call 45 every result carries a stop reminder,
     past the limit everything is refused except git, SendMessage and writing the REMAINDER/report;
  3. no `sleep` longer than 270 s in one call (the prompt cache expires at 5 minutes);
  4. at most two editor builds per worker;
  5. one `make` on the machine at a time, at most -j4 (16 GB RAM; 2026-09-24 parallel builds OOM-killed it);
  6. one editor/runtime process (GPU) at a time (2026-09-24 concurrent editors hung WindowServer: kernel panic);
  7. economy (ledger 2026-09-24: shell search+read = 50-80 % of cost): Explore only on haiku; no code read before
     the agent has read .claude/CODEMAP.md or asked Explore; code read in ranges of <= 150 lines, never `cat` whole.

The lead's own session (no agent id in the hook input) and Explore agents are never restricted.
Every decision is appended to ~/.claude/agent-guard/log.jsonl so the effect can be measured.
"""
import json
import os
import re
import subprocess
import sys
import time

STATE_DIR = os.path.expanduser("~/.claude/agent-guard")  # survives a reboot: /tmp reset the 80-call budget
TURN_WARN = 45
TURN_LIMIT = 60
TURN_HARD_CAP = 60  # owner 2026-09-29: the rest goes to a FRESH agent with REMAINDER file:line, never an extension
MAX_SLEEP = 270
MAX_EDITOR_BUILDS = 2

TREE_SEARCH = [
    re.compile(r"(^|[;&|(]\s*|\s)(grep|egrep|fgrep)\s+(-[A-Za-z]*[rR][A-Za-z]*|--recursive)\b"),
    re.compile(r"(^|[;&|(]\s*|\s)rg\s"),
    re.compile(r"(^|[;&|(]\s*|\s)git\s+grep\b"),
    re.compile(r"(^|[;&|(]\s*|\s)(ag|ack)\s"),
]
FIND = re.compile(r"(^|[;&|(]\s*|\s)find\s")
# grep -r over a build log directory or the scratch is reading logs, not searching the tree (09-29: L10b refused)
LOG_SEARCH = re.compile(r"grep\s+-[A-Za-z]*[rR][A-Za-z]*\s+(\S+\s+)?[\"']?(\S*build/DevLogs|/private/tmp/|/tmp/)")
SLEEP = re.compile(r"\bsleep\s+(\d+)")
BRIEF_PATH = re.compile(r"/[^\s'\";|&]*BRIEF\.md")
# the body of a heredoc (python/C++ written to a file) is data, not shell: `str.find(` is not a tree search
HEREDOC_BODY = re.compile(r"<<-?\s*['\"]?(\w+)['\"]?[^\n]*\n.*?\n\1\b", re.S)
EDITOR_BUILD = re.compile(r"((^|[;&|(]\s*|\s)make\s|build_quiet\.sh\s)[^;&|]*\bEditor\b")  # make as a COMMAND: `ls Desert.make Editor.make` counted as a build
SINGLE_TU = re.compile(r"\.o\b|\s-n\b|--dry-run")
MAKE = re.compile(r"(^|[;&|(]\s*|\s)make\s")
MAKE_JOBS = re.compile(r"\bmake\b[^;&|]*?-j\s*(\d+)")
MAX_MAKE_JOBS = 4
SELF_WAIT = re.compile(r"pgrep\s+-x\s+make|build_quiet\.sh")  # a command that waits for the other build itself is allowed
GIT_COMMIT = re.compile(r"\bgit\b[^;&|]*\bcommit\b")
WIP_COMMIT = re.compile(r"\bgit\b[^;&|]*\bcommit\b[^;&|]*(-m\s*[\"']wip|-F\s*-\s*<<-?\s*[\"']?\w+[\"']?\s*\n\s*wip)", re.I)
CODE_FILE = re.compile(r"\.(cpp|hpp|h|glslh|shader|mm)\b")
CODE_READ = re.compile(r"(^|[;&|(]\s*)(cat|head|tail|sed|grep|awk|less|more)\s")
MAX_READ_LINES = 150
WORKTREE = re.compile(r"(/Users/[^\s\"']+/DesertEngine-[A-Za-z0-9]+)(?:/|\b)")
EDITOR_RUN = re.compile(r"Bin/(Debug|Release)/(Editor|Runtime)\b")
# Owner 2026-09-29: an agent never waits for CI — the idle cache expires and the whole context is written again
# (CI12: 1.5 of 3.2 M units). Push, report the run id, the lead watches it from a background shell for free.
CI_WAIT = re.compile(r"\bgh\s+(run\s+watch|pr\s+checks\b[^;&|]*--watch)|"
                     r"\b(while|until|for)\b[^\n]*\bgh\s+(run|pr)\b|\bgh\s+(run|pr)\b[^\n]*\bsleep\b")


def runs_editor(cmd):
    """True only when a shell segment EXECUTES the editor/runtime binary. The bare path regex also caught
    `cp .../Editor.exe`, `test -f .../Editor`, a path passed as an argument (reported from Windows 09-27),
    and agents worked around it with variables. A segment executes it when its first word (after
    VAR=value assignments and exec/time/nohup/env) is the binary."""
    for segment in re.split(r"[;&|\n]+|\$\(|`", cmd):
        words = segment.strip().split()
        while words and (re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", words[0]) or words[0] in ("exec", "time", "nohup", "env", "command")):
            words = words[1:]
        if words and words[0] in ("python", "python3", "py") and len(words) > 1 and "run_capped" in words[1]:
            continue
        if words and EDITOR_RUN.search(words[0].strip("\"'")):
            return True
    return False
ALWAYS_ALLOWED_AFTER_LIMIT =re.compile(r"^\s*(cd [^;&]+&&\s*)?git\s")


# Owner 2026-09-29: every refused call re-sends the whole context (~2.9k refusals = ~7 % of spend). The four
# commonest refusals (tree search 81, cat 53, push without handoff 46, code before map 36 in 24 h) are answered
# BEFORE the agent tries them: the allowed form of each is put into its context with the map, after call 1.
CHEAT_SHEET = """[agent_guard] РАЗРЕШЁННЫЕ ФОРМЫ (каждый отказ хука стоит полного вызова — не пробуй запрещённое):
- где определено имя: scripts/Dev/sym.sh <Имя>; где используется: scripts/Dev/sym.sh --refs <Имя>. НЕ grep -r / rg / git grep / find без -maxdepth.
- чтение кода: grep -n <шаблон> <известный файл> → sed -n 'A,Bp' <файл> (≤150 строк) или Read(offset, limit≤150). НЕ cat / Read целиком.
- тесты: scripts/Dev/suite.sh <Сюита…>. Сдача: последний коммит с темой «wip: …» → git push (полный handoff_check гоняет тимлид; не-wip без .cache/handoff/<HEAD>.ok хук откажет).
- dev вливается только scripts/Dev/merge_dev.sh; сцены — scripts/Dev/migrate.sh; редактор — через run_capped.
- сборка: build_quiet.sh в фоне + build_wait.sh; одна make на машине, -j≤4; sleep ≤ 270 с.
- формат диффа: /opt/homebrew/opt/llvm@18/bin/git-clang-format --binary /opt/homebrew/opt/llvm@18/bin/clang-format <база> (git-clang-format из PATH — v22, падает на -list-ignored; clang-format -i по файлу целиком НЕ запускать).
- долгое (> 4 мин: сюиты, мигратор, CheckTidy, сборка) — run_in_background + ~/.claude/tools/wait_bg.sh <output-файл> (≤ 4 мин за вызов); timeout > 280 с — отказ, ход в ожидании уведомления не заканчивать.
- CI не ждёшь: push → id прогона в отчёт → конец. Лимит 60 вызовов без продлений: остаток — REMAINDER.md в скретче."""



def bg_out(data):
    """The output file of a background Bash job, read from the tool's own reply, so the hint names the exact wait."""
    m = re.search(r"Output is being written to: (\S+)", json.dumps(data.get("tool_response", "")))
    return m.group(1).rstrip("\\.\"") if m else "<output-файл из ответа инструмента>"

def map_digest(map_path=None, tree=None):
    """CODEMAP's index (every '## ' heading with its line number) and its hand-written Notes section."""
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    map_path = map_path or os.path.join(root, ".claude", "CODEMAP.md")
    try:
        lines = open(map_path, encoding="utf-8").read().split("\n")
    except OSError:
        return "[agent_guard] .claude/CODEMAP.md is missing — tell the lead; do not search the tree instead."
    index = [f"{i + 1}: {l}" for i, l in enumerate(lines) if l.startswith("## ")]
    notes, inside = [], False
    for l in lines:
        if l.startswith("## "):
            inside = l.startswith("## Notes")
            continue
        if inside and l.strip():
            notes.append(l)
    if tree:
        return (f"[agent_guard] КАРТА ТВОЕЙ ВЕТКИ (дерево {tree}) построена заново: {map_path} — в ней и новые папки "
                f"задачи. Читай СВОЙ раздел диапазоном: sed -n 'A,Bp' {map_path}. Разделы (строка: заголовок):\n" +
                "\n".join(index))
    return ("[agent_guard] КАРТА ПРОЕКТА (.claude/CODEMAP.md). Разделы (строка: заголовок):\n" + "\n".join(index) +
            "\nЗаметки карты:\n" + "\n".join(notes[:40]) +
            "\nПрочитай СВОЙ раздел: sed -n 'A,Bp' .claude/CODEMAP.md (A..B — от его заголовка до следующего) и строку "
            "своего .cpp в «Source → suites». До этого чтение кода отклоняется. Поиск по многим файлам — Explore на "
            "haiku (Agent subagent_type \"Explore\", model \"haiku\"): его чтение не ложится в твой контекст.\n" + CHEAT_SHEET)


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
                                "tool": data.get("tool_name"), "decision": decision, "note": note[:200],
                                # every call's command (deny: 160 chars, to find false refusals; allow: 100, to see where
                                # search/read go — 09-29 night: search 37-41 % with a map in the brief, cause unknowable without it)
                                "cmd": str((data.get("tool_input") or {}).get("command")
                                           or (data.get("tool_input") or {}).get("file_path")
                                           or (data.get("tool_input") or {}).get("pattern") or "")[:160 if decision == "deny" else 100]}) + "\n")
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
        # -x on the process NAME: `pgrep -f <path>` also matched the waiting shell's own command line and spun
        return any(subprocess.run(["pgrep", "-x", n], capture_output=True).returncode == 0 for n in ("Editor", "Runtime"))
    except OSError:
        return False


def staged_claude_files(cmd, cwd):
    """True when the commit about to run would carry files under .claude/ (staged, or `-a` on modified ones)."""
    m = re.search(r"\bcd\s+(\"[^\"]+\"|'[^']+'|\S+)", cmd) or re.search(r"\bgit\s+-C\s+(\"[^\"]+\"|\S+)", cmd)
    where = m.group(1).strip("\"'") if m else (cwd or ".")
    args = ["git", "-C", where, "diff", "--name-only"]
    try:
        staged = subprocess.run(args + ["--cached"], capture_output=True, text=True).stdout.split()
        if re.search(r"\bcommit\b[^;&|]*\s-[A-Za-z]*a", cmd):
            staged += subprocess.run(args, capture_output=True, text=True).stdout.split()
    except OSError:
        return False
    return any(f.startswith(".claude/") for f in staged)


REPORT_MAX_LINES = 20


def last_report_text(data):
    """The sub-agent's final message: newer hook inputs carry it, older ones only the transcript path."""
    text = data.get("last_assistant_message")
    if isinstance(text, str) and text.strip():
        return text
    for key in ("agent_transcript_path", "transcript_path"):
        p = data.get(key)
        if not p or not os.path.exists(p):
            continue
        last = ""
        with open(p, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                try:
                    m = json.loads(line).get("message", {})
                except ValueError:
                    continue
                if m.get("role") == "assistant":
                    parts = [c.get("text", "") for c in m.get("content", []) if isinstance(c, dict) and c.get("type") == "text"]
                    if any(t.strip() for t in parts):
                        last = "\n".join(parts)
        if last:
            return last
    return ""


def subagent_stop(data):
    """Owner 2026-09-24: every report line is paid again in every later turn of the lead. One retry only."""
    if (data.get("agent_type") or "").lower() == "explore" or data.get("stop_hook_active"):
        sys.exit(0)
    lines = [l for l in last_report_text(data).splitlines() if l.strip()]
    if len(lines) > REPORT_MAX_LINES:
        log(data, data.get("agent_id") or "?", "block", f"report {len(lines)} lines")
        emit({"decision": "block", "reason":
              f"[agent_guard] Отчёт {len(lines)} непустых строк, предел {REPORT_MAX_LINES}. Подробности (таблицы, логи, "
              f"списки файлов) запиши в файл в своей папке и перепиши отчёт: итог, цифры, путь к файлу, остаток."})
    sys.exit(0)


GIT_PUSH = re.compile(r"\bgit\b[^;&|]*\bpush\b")
DEV_MERGE = re.compile(r"\bgit\b[^;&|]*\b(merge|pull)\b(?!-)[^;&|]*\b(origin/dev|origin\s+dev|\bdev)\b")
MIGRATOR_RUN = re.compile(r"Bin/(Debug|Release)/SceneMigrator\b")
TEST_LOOP = re.compile(r"RunTests\.sh|for\s+\w+\s+in\s+[^;]*Bin/Tests/")


def target_tree(cmd, cwd):
    """The tree a git command acts on: `git -C <dir>`, else the last `cd <dir>` before it, else the hook's cwd."""
    m = re.search(r"\bgit\s+-C\s+(\"[^\"]+\"|'[^']+'|\S+)", cmd)
    if m:
        return m.group(1).strip("\"'")
    cds = re.findall(r"(?:^|[;&|(]\s*)cd\s+(\"[^\"]+\"|'[^']+'|\S+)", cmd)
    return cds[-1].strip("\"'") if cds else cwd


def script_rule(cmd, cwd):
    """A refusal text when a routine step bypasses its scripts/Dev script, else None. A tree whose branch predates
    scripts/Dev (cut before 2026-09-24 evening) is not held to it — it has nothing to call until it merges dev."""
    tree = target_tree(cmd, cwd)
    has = lambda name: os.path.exists(os.path.join(tree, "scripts", "Dev", name))
    if GIT_PUSH.search(cmd) and "--delete" not in cmd and has("handoff_check.sh"):
        head = subprocess.run(["git", "-C", tree, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        subject = subprocess.run(["git", "-C", tree, "log", "-1", "--format=%s"], capture_output=True, text=True).stdout
        if WIP_COMMIT.search(cmd):  # `git commit -m "wip: …" && git push` — HEAD is still the old commit at check time
            subject = "wip"
        if head and not subject.lower().startswith("wip") and \
                not os.path.exists(os.path.join(tree, ".cache", "handoff", head + ".ok")):
            return ("[agent_guard] Push без проверки: нет .cache/handoff/<HEAD>.ok. Запусти scripts/Dev/handoff_check.sh "
                    "(в фоне, ~6 мин) — он пишет маркер только на зелёном и чистом дереве. Не успеваешь — коммит с "
                    "темой «wip: …» пушится без маркера, и тимлид знает, что ветка не сдана.")
    if DEV_MERGE.search(cmd) and "merge_dev.sh" not in cmd and has("merge_dev.sh"):
        return ("[agent_guard] dev вливается только scripts/Dev/merge_dev.sh: он останавливается на конфликте и ловит "
                "расхождение .claude (агент не может его закоммитить, и слияние оставляло старый хук).")
    if MIGRATOR_RUN.search(cmd) and "DESERT_MIGRATE_VIA_SCRIPT=1" not in cmd and "migrate.sh" not in cmd and has("migrate.sh"):
        return ("[agent_guard] SceneMigrator — только через scripts/Dev/migrate.sh [--write] <пути>: он пересобирает "
                "Debug-бинарник и отказывается от протухшего (протухший Release однажды снёс 101k строк в 157 файлах).")
    if TEST_LOOP.search(cmd) and "handoff_check.sh" not in cmd and "suite.sh" not in cmd and has("suite.sh"):
        return ("[agent_guard] Тесты — scripts/Dev/suite.sh <Сюита…> или scripts/Dev/handoff_check.sh (все бинарники "
                "из корня); RunTests.sh падает на двоичном выводе, свой цикл запускает не из корня.")
    return None


def self_check():
    """SessionStart: prove every rule still bites by replaying known-bad calls; a silent regression of this file
    (2026-09-24: a merge restored an old copy and the build limits vanished) must be loud at the next start."""
    cases = {
        "tree search": {"tool_name": "Bash", "tool_input": {"command": "grep -rn x ."}},
        "find without depth": {"tool_name": "Bash", "tool_input": {"command": "find . -name x"}},
        "long sleep": {"tool_name": "Bash", "tool_input": {"command": "sleep 600"}},
        "make -j8": {"tool_name": "Bash", "tool_input": {"command": "make Editor -j8"}},
        "editor without cap": {"tool_name": "Bash", "tool_input": {"command": "cd Editor && ../build/Bin/Debug/Editor"}},
        "explore not on haiku": {"tool_name": "Agent", "tool_input": {"subagent_type": "Explore", "prompt": "x"}},
        "agent spawns a worker": {"tool_name": "Agent", "tool_input": {"subagent_type": "general-purpose", "prompt": "x"}},
        "clean the build": {"tool_name": "Bash", "tool_input": {"command": "make -f Desert.make clean"}},
        "code before map": {"tool_name": "Bash", "tool_input": {"command": "grep -n Foo Desert/X.cpp"}},
        "whole-file Read": {"tool_name": "Read", "tool_input": {"file_path": "/x/Desert/X.cpp"}},
        "edit .claude": {"tool_name": "Edit", "tool_input": {"file_path": "/x/.claude/tools/agent_guard.py"}},
        "push without handoff": {"tool_name": "Bash", "tool_input": {"command": "git -C " + os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))) + " push origin nothing-selfcheck"}},
        "merge dev by hand": {"tool_name": "Bash", "tool_input": {"command": "git merge origin/dev"}},
        "call longer than the cache": {"tool_name": "Bash", "tool_input": {"command": "scripts/Dev/suite.sh X",
                                                                           "timeout": 600000}},
        "migrator directly": {"tool_name": "Bash", "tool_input": {"command": "./build/Bin/Debug/SceneMigrator a"}},
        "own test loop": {"tool_name": "Bash", "tool_input": {"command": "bash scripts/MacOS/RunTests.sh"}},
        "agent waits for CI": {"tool_name": "Bash", "tool_input": {"command": "gh run watch 1 --exit-status"}},
        "CI poll loop": {"tool_name": "Bash", "tool_input": {"command": "for i in $(seq 20); do gh run view 1 --json status; sleep 10; done"}},
    }
    failed = []
    for name, case in cases.items():
        payload = dict(case, agent_id="selfcheck-" + name.replace(" ", "-"), agent_type="general-purpose",
                       hook_event_name="PreToolUse",
                       cwd=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
        out = subprocess.run([sys.executable, __file__], input=json.dumps(payload), capture_output=True, text=True)
        if '"deny"' not in out.stdout:
            failed.append(name)
        try:
            os.remove(os.path.join(STATE_DIR, payload["agent_id"] + ".json"))
        except OSError:
            pass
    rules = ", ".join(cases)
    msg = (f"[agent_guard] self-check OK: {len(cases)} known-bad calls refused ({rules})." if not failed else
           f"[agent_guard] SELF-CHECK FAILED — these rules no longer bite: {', '.join(failed)}. "
           f"Restore .claude/tools/agent_guard.py from git history BEFORE launching any agent.")
    emit({"hookSpecificOutput": {"hookEventName": "SessionStart", "additionalContext": msg}})


def main():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        sys.exit(0)

    if data.get("hook_event_name") == "SubagentStop":
        subagent_stop(data)  # before the agent-id test: a SubagentStop input may not carry one

    # Verified 2026-09-24 on live calls: a sub-agent's input carries agent_id and agent_type.
    agent = data.get("agent_id")
    agent_type = (data.get("agent_type") or "").lower()
    if not agent or agent_type == "explore":
        sys.exit(0)  # the lead's own session, or a discovery agent: unrestricted

    event = data.get("hook_event_name", "PreToolUse")
    state, path = load_state(agent)

    # A continued agent (the lead resumed it with SendMessage for the next step in the same code) gets a larger
    # budget from ~/.claude/tools/agent_extend.py: re-reading the same files in a fresh agent was 40-60 % of a step.
    limit = state.get("limit", TURN_LIMIT)
    if not state.get("cap_exempt"):  # set by hand only for an agent launched before the 09-29 cap
        limit = min(limit, TURN_HARD_CAP)
    if event == "PostToolUse":
        # Owner 2026-09-28: «надо гарантировать что агенты прочитают карту проекта». The map's index (section
        # headings with line numbers) and its hand-written notes are put into the agent's context after its
        # FIRST call, so no agent starts without them; the code-read rule below still demands its own section.
        if not state.get("map_shown"):
            state["map_shown"] = True
            save_state(state, path)
            emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": map_digest()}})
            sys.exit(0)
        pin = data.get("tool_input") or {}
        # Owner 2026-09-29 («может агент автоматически будет её строить?»): the injected map is dev's, and a task
        # branch's new folders are not in it (the night's continuation agents searched 30-46 % for them). The first
        # call that names a worktree (/…/DesertEngine-<X>/) builds THAT tree's map (0.3 s, no tokens) and points to it.
        if not state.get("branch_map"):
            tm = WORKTREE.search(pin.get("command", "") or pin.get("file_path", "") or "")
            if tm and os.path.isdir(tm.group(1)):
                out = os.path.join(STATE_DIR, "maps", agent + "-CODEMAP.md")
                os.makedirs(os.path.dirname(out), exist_ok=True)
                gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen_codemap.py")
                subprocess.run([sys.executable, gen], cwd=tm.group(1), env=dict(os.environ, CODEMAP_OUT=out),
                               capture_output=True, timeout=30)
                state["branch_map"] = out
                save_state(state, path)
                if os.path.exists(out):
                    emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext":
                          map_digest(out, tm.group(1))}})
        if data.get("tool_name") == "Bash" and pin.get("run_in_background"):
            # Any background job, not only build_quiet.sh: T6c5 ran suite.sh in the background and went idle.
            emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext":
                  "[agent_guard] Задача в фоне. НЕ заканчивай ход в ожидании уведомления: простой > 5 мин сбрасывает кэш "
                  "контекста (AF7v потерял так 0,43 млн). Жди блокирующими вызовами ≤ 4 мин: сборка build_quiet.sh — "
                  "~/.claude/tools/build_wait.sh <лог>; прочее — ~/.claude/tools/wait_bg.sh " + bg_out(data) + "."}})
            sys.exit(0)
        calls = state.get("calls", 0)
        if calls >= limit - (TURN_LIMIT - TURN_WARN):
            emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext":
                  f"[agent_guard] Вызов {calls}/{limit}, осталось {limit - calls}. Доделай ТЕКУЩИЙ шаг, "
                  f"новый крупный не начинай; закоммить, запушь и отчитайся до {limit}. Не останавливайся "
                  f"раньше времени. Остаток — в REMAINDER.md в своём скретче (файл:строка, что заменить на что, что проверено): "
                  f"продлений нет, его доделает свежий агент по этому списку."}})
        sys.exit(0)

    tool = data.get("tool_name", "")
    tin = data.get("tool_input") or {}
    cmd = tin.get("command", "") if tool == "Bash" else ""

    state["calls"] = state.get("calls", 0) + 1
    calls = state["calls"]

    if calls > limit:
        allowed = tool in ("SendMessage", "Write") or (tool == "Bash" and ALWAYS_ALLOWED_AFTER_LIMIT.match(cmd))
        if not allowed:
            save_state(state, path)
            deny(f"[agent_guard] Лимит {limit} вызовов исчерпан ({calls}). Закоммить и запушь сделанное, запиши "
                 f"REMAINDER.md в скретч (файл:строка) и отчитайся тимлиду; остаток он отдаст свежему агенту.", data, agent)

    # Owner's rule: at most three programme agents. A sub-agent that spawns a general worker is a fourth
    # (2026-09-24: P10e spawned "P10e code" and the machine ran five). Only Explore (discovery) is allowed.
    # Owner 2026-09-24 refined it: the limit exists for ECONOMY — a helper is fine when it LOWERS spend. So a worker
    # is allowed only on a cheaper model than the caller (sonnet or haiku), never on the inherited expensive one.
    if tool == "Agent" and (tin.get("subagent_type") or "general-purpose").lower() != "explore" and \
            (tin.get("model") or "").lower() not in ("sonnet", "haiku"):
        save_state(state, path)
        deny("[agent_guard] Помощник допустим, только если он СНИЖАЕТ расход: model: \"sonnet\" или \"haiku\" "
             "(механика, прогоны, правки по списку). На своей модели — делай сам; не влезает — коммит, пуш, отчёт.",
             data, agent)

    # The build tree is shared state and costs a full rebuild (~10 min, a dozen calls of waiting) to recreate.
    if tool == "Bash" and re.search(r"\bmake\b[^;&|]*\sclean(\s|$|;|&)|\brm\s+-[a-zA-Z]*r[a-zA-Z]*\s+[^;&|]*\bbuild(/|\s|$)", cmd) \
            and not re.search(r"\brm\s+-[a-zA-Z]*\s+[^;&|]*build/Tests/Intermediates/\w+/\w+/\w+", cmd):
        save_state(state, path)
        deny("[agent_guard] Дерево сборки не чистится: make clean / rm -rf build стоит полной пересборки. Устаревший "
             "объект — пересобери один файл (touch источника) или удали один .o.", data, agent)

    # --- Economy rules measured on the ledger (owner 2026-09-24: «сделай так, чтобы агенты опять не НЕ исполнили») ---
    if tool == "Agent" and (tin.get("subagent_type") or "").lower() == "explore" and \
            (tin.get("model") or "").lower() != "haiku":
        save_state(state, path)
        deny("[agent_guard] Explore запускается на haiku: Agent(subagent_type: \"Explore\", model: \"haiku\", ...). "
             "Он только находит файлы; дорогая модель тут — пустая трата.", data, agent)
    # A SECTION read counts (sed -n 'A,Bp' / Read with an offset), not a mere mention: `grep -n '^## '` alone
    # listed the headings and let the agent go on searching (ledger 09-28: search+read still 55-80 %).
    if (tool == "Bash" and "CODEMAP.md" in cmd and re.search(r"sed\s+-n\s+'?\d+,\d+p", cmd)) or \
            (tool == "Read" and (tin.get("file_path") or "").endswith("CODEMAP.md") and tin.get("offset")):
        state["map_read"] = True
    # The lead's pre-scan puts a «## Карта» into the brief (LEAD_PROTOCOL §000 step 2): reading THAT brief is reading the
    # map — refusing the first code read then only cost a call (09-29: L10b2, PRJ1a).
    brief = BRIEF_PATH.search(cmd if tool == "Bash" else (tin.get("file_path") or ""))
    if brief and not state.get("map_read"):
        try:
            if "## Карта" in open(brief.group(0), encoding="utf-8").read():
                state["map_read"] = True
        except OSError:
            pass
    reading_code = (tool == "Read" and CODE_FILE.search(tin.get("file_path") or "")) or \
                   (tool == "Bash" and CODE_READ.search(cmd) and CODE_FILE.search(cmd))
    if reading_code and not state.get("map_read") and not state.get("explored"):
        save_state(state, path)
        deny("[agent_guard] Сначала карта: прочитай свой раздел .claude/CODEMAP.md (grep -n '^## ' .claude/CODEMAP.md, "
             "затем sed -n по диапазону) или спроси Explore на haiku — и только потом читай код.", data, agent)
    if tool == "Agent" and (tin.get("subagent_type") or "").lower() == "explore":
        state["explored"] = True
    if tool == "Read" and CODE_FILE.search(tin.get("file_path") or "") and \
            (not tin.get("limit") or int(tin.get("limit") or 0) > MAX_READ_LINES):
        save_state(state, path)
        deny(f"[agent_guard] Код читается диапазоном ≤ {MAX_READ_LINES} строк: Read(file_path, offset, limit≤{MAX_READ_LINES}) "
             f"или sed -n 'A,Bp'. Целый файл — главная статья расхода.", data, agent)
    if tool == "Bash" and CODE_FILE.search(cmd):
        if re.search(r"(^|[;&|(]\s*)cat\s+(?!>)[^|;&>]*\.(cpp|hpp|h|glslh|shader|lua|mm)\b", cmd):
            save_state(state, path)
            deny(f"[agent_guard] `cat` исходника целиком запрещён: grep -n → sed -n 'A,Bp' (≤ {MAX_READ_LINES} строк).",
                 data, agent)
        for a, b in re.findall(r"sed\s+-n\s+['\"]?(\d+),(\d+)p", cmd):
            if int(b) - int(a) > MAX_READ_LINES:
                save_state(state, path)
                deny(f"[agent_guard] sed -n {a},{b}p — {int(b) - int(a)} строк > {MAX_READ_LINES}. Читай уже.", data, agent)

    if tool in ("Edit", "Write", "NotebookEdit") and "/.claude/" in (tin.get("file_path") or ""):
        save_state(state, path)
        deny("[agent_guard] Файлы .claude/ (хуки, бриф, контракт) правит только тимлид: 2026-09-24 слияние "
             "принесло старую копию хука из дерева агента, лимиты сборок исчезли, машина упала.", data, agent)
    # --- scripts/Dev is the only way to do the routine (owner 2026-09-24: «чтобы агенты всегда точно их использовали») ---
    if tool == "Bash":
        denial = script_rule(cmd, data.get("cwd") or "")
        if denial:
            save_state(state, path)
            deny(denial, data, agent)

    if tool == "Bash" and GIT_COMMIT.search(cmd) and staged_claude_files(cmd, data.get("cwd")):
        save_state(state, path)
        deny("[agent_guard] В коммите есть файлы .claude/ — их коммитит только тимлид. Убери их из индекса: "
             "git restore --staged .claude && git checkout -- .claude", data, agent)

    if tool == "Bash":
        shell = HEREDOC_BODY.sub("", cmd)
        for rx in TREE_SEARCH:
            if rx.search(shell) and not LOG_SEARCH.search(shell):
                save_state(state, path)
                deny("[agent_guard] Поиск по дереву в своём контексте запрещён (контракт §7.3). Где определено имя — "
                     "scripts/Dev/sym.sh <Имя> (1–2 с), где используется — sym.sh --refs <Имя>. Вопрос шире — "
                     "субагенту: Agent(subagent_type: \"Explore\", prompt: \"<что найти, ответ ≤60 строк>\"). "
                     "Сам ищи только внутри уже известных файлов: grep -n <шаблон> <файл>.", data, agent)
        if FIND.search(shell) and "-maxdepth" not in shell:
            save_state(state, path)
            deny("[agent_guard] `find` без -maxdepth — это поиск по дереву; используй Explore или "
                 "`find <папка> -maxdepth 2 ...`.", data, agent)
        for m in SLEEP.finditer(cmd):
            if int(m.group(1)) > MAX_SLEEP:
                save_state(state, path)
                deny(f"[agent_guard] sleep {m.group(1)} > {MAX_SLEEP} с: кэш истекает через 5 минут и весь "
                     f"контекст пишется заново. Жди кусками: for i in $(seq 27); do grep -q <маркер> <лог> && "
                     f"break; sleep 10; done", data, agent)
        # A foreground call longer than the cache's 5 minutes re-reads the whole context (09-29: L10a2 lost 299k,
        # 30 %, on 6-8 min calls). Long work goes to the background and is waited on in <= 4-min calls.
        if not tin.get("run_in_background") and int(tin.get("timeout") or 0) > MAX_SLEEP * 1000 + 10000:
            save_state(state, path)
            deny(f"[agent_guard] timeout {int(tin['timeout']) // 1000} с > {MAX_SLEEP + 10} с: вызов дольше 5 минут "
                 "сбрасывает кэш контекста (перечитывание ~1,25× всего контекста). Запусти то же с run_in_background: "
                 "true и жди ~/.claude/tools/wait_bg.sh <output-файл> (≤ 4 мин за вызов; сборка — build_wait.sh).",
                 data, agent)
        if CI_WAIT.search(cmd):
            save_state(state, path)
            deny("[agent_guard] CI не ждёшь сам: пауза сбрасывает кэш, и весь контекст пишется заново (CI12: 1,5 из "
                 "3,2 млн). Запушь, отчитайся с id прогона (gh run list --branch <ветка> --limit 1, один раз) и "
                 "заканчивай — прогон смотрит тимлид.", data, agent)
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
        if runs_editor(cmd) and "run_capped" not in cmd:
            save_state(state, path)
            deny("[agent_guard] Редактор/рантайм запускается только через ограничитель памяти: "
                 "~/.claude/tools/run_capped.sh ../build/Bin/Debug/Editor ... на macOS "
                 "(путь: /Users/daniilsavcenko/.claude/tools/run_capped.sh), "
                 "python .claude/tools/run_capped.py build/Bin/Debug/Editor.exe ... на Windows. "
                 "2026-09-24 один редактор съел 13.7 ГБ из 16 и уронил машину.", data, agent)
        if (runs_editor(cmd) or "run_capped" in cmd) and editor_running():
            save_state(state, path)
            deny("[agent_guard] Уже запущен редактор/рантайм (другой агент). Одновременно — только ОДИН процесс с GPU: "
                 "2026-09-24 несколько редакторов повесили WindowServer, ядро ушло в panic. Подожди в той же команде: "
                 "for i in $(seq 27); do pgrep -x Editor >/dev/null || pgrep -x Runtime >/dev/null || break; "
                 "sleep 10; done; <запуск>", data, agent)
        # a single translation unit (`make -f Editor.make .../EditorLayer.o`) is the cheap check the brief asks for
        # BEFORE a full build; counting it made the rule punish its own advice (AF9k, 2026-09-24)
        if EDITOR_BUILD.search(cmd) and not SINGLE_TU.search(cmd):
            if state.get("editor_builds", 0) >= MAX_EDITOR_BUILDS:
                save_state(state, path)
                deny(f"[agent_guard] Editor уже собирался {MAX_EDITOR_BUILDS} раза в этой задаче. Итерации — на "
                     f"сюитах; если без третьей сборки нельзя — SendMessage тимлиду с причиной.", data, agent)
            state["editor_builds"] = state.get("editor_builds", 0) + 1

    save_state(state, path)
    log(data, agent, "allow")
    sys.exit(0)


if __name__ == "__main__":
    if "--self-check" in sys.argv:
        self_check()
    main()
