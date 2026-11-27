#!/usr/bin/env python3
"""agent_guard — the hook that ENFORCES how agents are launched and how they spend their context.

One process per hook event. `decide(data)` is a pure function of the event (plus the state files), so the
self-check replays known-bad calls in-process: no child processes, no log rows, one line of output.

Rules, by who makes the call (numbers: limits.py; pipeline and ownership: pipeline.py):
  lead     - launch: code in the prompt, ceiling of live agents, disk, no sonnet, no duplicate live run, stall,
             stale branch, unbuilt runs, brief of a fix (size, scenario, architecture), engineer only for a PLANNED
             package (design is a parallel plan; no file overlap with a live run; no `isolation`);
             own waste (timeout around tree-writing git, whole diff, huge output); Notion only through `board`;
             no SendMessage to a long agent; push rules. Launch / resume are registered in pipeline.json.
  board    - Notion tools only.
  explore  - unrestricted.
  worker   - call budget with a finishing window; no helper agents except Explore on haiku; map before code; code read
             by ranges; no tree search; no long sleep / foreground call; no .claude edits or commits; scripts/Dev for
             the routine; editor only through play_scenario; no asset removal; FILE OWNERSHIP inside a package tree.
  stop     - report length; the run is marked handed in; an architect's design must be a parallel plan.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import limits
import pipeline as P

for _stream in (sys.stdin, sys.stdout, sys.stderr):
    _stream.reconfigure(encoding="utf-8")

DRY = False  # the self-check: no state files, no log
READ_ONLY_AGENTS = ("explore", "board", "architect", "reviewer", "acceptance")  # no code edits: no work brief needed
FEATURE_LANE = ("engineer", "architect", "reviewer")
CODE_RE = r"[A-Z][A-Z0-9]*(?:-[A-Z0-9+]+)*"
ARCH_KEYS = ("Эталон", "Варианты", "Выбор")

TREE_SEARCH = [
    re.compile(r"(^|[;&|(]\s*|\s)(grep|egrep|fgrep)\s+(-[A-Za-z]*[rR][A-Za-z]*|--recursive)\b"),
    re.compile(r"(^|[;&|(]\s*|\s)rg\s"),
    re.compile(r"(^|[;&|(]\s*|\s)git\s+grep\b"),
    re.compile(r"(^|[;&|(]\s*|\s)(ag|ack)\s"),
]
FIND = re.compile(r"(^|[;&|(]\s*|\s)find\s")
LOG_SEARCH = re.compile(r"grep\s+-[A-Za-z]*[rR][A-Za-z]*\s+(\S+\s+)?[\"']?(\S*build/DevLogs|/private/tmp/|/tmp/)")
SLEEP = re.compile(r"\bsleep\s+(\d+)")
BRIEF_PATH = re.compile(r"(?:[A-Za-z]:)?/[^\s'\";|&]*BRIEF[A-Za-z0-9_-]*\.md")
DESIGN_PATH = re.compile(r"(?:[A-Za-z]:)?/[^\s'\"`;|&]*DESIGN[A-Za-z0-9_.-]*\.md")
HEREDOC_BODY = re.compile(r"<<-?\s*['\"]?(\w+)['\"]?[^\n]*\n.*?\n\1\b", re.S)  # a heredoc body is data, not shell
GIT_COMMIT = re.compile(r"\bgit\b[^;&|]*\bcommit\b")
WIP_COMMIT = re.compile(r"\bgit\b[^;&|]*\bcommit\b[^;&|]*(-m\s*[\"']wip|-F\s*-\s*<<-?\s*[\"']?\w+[\"']?\s*\n\s*wip)", re.I)
GIT_PUSH = re.compile(r"\bgit\b[^;&|]*\bpush\b([^;&|]*)")
DEV_MERGE = re.compile(r"\bgit\b[^;&|]*\b(merge|pull)\b(?!-)[^;&|]*(\borigin/dev\b|\borigin\s+dev\b|(?<![/\w])dev\b)")
MIGRATOR_RUN = re.compile(r"Bin/(Debug|Release)/SceneMigrator\b")
TEST_LOOP = re.compile(r"RunTests\.sh|run_tests\.bat|for\s+\w+\s+in\s+[^;]*Bin/Tests/")
CODE_FILE = re.compile(r"\.(cpp|hpp|h|glslh|shader|mm)\b")
CODE_READ = re.compile(r"(^|[;&|(]\s*)(cat|head|tail|sed|grep|awk|less|more)\s")
WORKTREE = re.compile(r"(/Users/[^\s\"']+/DesertEngine-[A-Za-z0-9]+"
                      r"|(?:[A-Za-z]:|/[a-z])[/\\](?:DesertEngine-[A-Za-z0-9]+|wt[/\\][A-Za-z0-9_.-]+))(?=[/\\\s\"']|$)")
EDITOR_RUN = re.compile(r"Bin/(Debug|Release)/(Editor|Runtime)\b")
LEAD_ONLY_RUNS = re.compile(r"scripts/CI/CheckTidy\.sh|scripts/CI/CheckGluedText\.sh|handoff_check\.sh")
CI_WAIT = re.compile(r"\bgh\s+(run\s+watch|pr\s+checks\b[^;&|]*--watch)|"
                     r"\b(while|until|for)\b[^\n]*\bgh\s+(run|pr)\b|\bgh\s+(run|pr)\b[^\n]*\bsleep\b")
UNCOUNTED_WAIT = re.compile(r"^\s*python3?\s+\S*studio\.py\s+wait\b")
ALWAYS_ALLOWED_AFTER_LIMIT = re.compile(r"^\s*(cd [^;&]+&&\s*)?git\s")
ASSET_RM = re.compile(r"(\b(rm|unlink|shred)\b[^;&|]*|\bfind\b[^;&|]*-delete\b[^;&|]*|\bxargs\b[^;&|]*\brm\b[^;&|]*)"
                      r"(Content/|\.(demat|stmesh|skmesh|detex|desce|deimport|deanim|deprefab|decloudtype|dclayout)\b)")
BUILD_RM = re.compile(r"\brm\s+-[a-zA-Z]*r[a-zA-Z]*\s+[^;&|]*\bbuild(/|\s|$)")
GIT_TREE_WRITE = re.compile(r"\btimeout\s+(?:-\S+\s+)*\d+\w?\s+git\s+(?:-C\s+\S+\s+)?"
                            r"(?:merge(?!-tree|-base)|rebase|checkout|switch|reset|stash|cherry-pick|am)\b")
GIT_TEXT = re.compile(r"\bgit\s+(?:-C\s+\S+\s+)?(?:diff|show|log\s+-p)\b")
CAPPED = re.compile(r"--(?:short)?stat|--name-only|--name-status|--numstat|--raw|--quiet|\|\s*(?:wc|grep|head|tail)\b|"
                    r">\s*[\w/$\"']|\s-s\b")
AGENT_ID = re.compile(r"agentId\"?\s*[:=]\s*\"?([0-9a-z]{8,})")
RESUMED_ID = re.compile(r"resumedAgentId\"?\s*[:=]\s*\"?([0-9a-z]{8,})")


def cheat_sheet(lane):
    warn, soft, hard = lane
    return (f"""[agent_guard] ПРАВИЛА ЗАХОДА (каждый отказ хука стоит полного вызова — не пробуй запрещённое):
- архитектура: эталон кода — паттерны C++ (навык desert-engine-architecture); UE — только вид и техники рендера. Без бюджетов-заглушек, мостов и угадываний; не влезает — REMAINDER.md, не компромисс.
- имя: scripts/Dev/sym.sh <Имя> (определение), sym.sh --refs <Имя> (использования). НЕ grep -r / rg / git grep / find без -maxdepth.
- код читается диапазоном: grep -n <шаблон> <файл> → Read(offset, limit) или sed -n 'A,Bp'. НЕ cat и не Read целиком.
- пакет фичи: правишь только файлы строки «Файлы:» своего брифа; заголовок контракта не трогаешь (CONTRACT-BREAK в отчёт); чужой файл — NEEDS в отчёт.
- не собираешь, не запускаешь тесты, не ждёшь CI: коммит «wip: <КОД> код готов» + push; сборка пачки и приёмка — у лида. Перед сдачей: python scripts/Dev/arch_census.py.
- dev вливается только scripts/Dev/merge_dev.sh; сцены — scripts/Dev/migrate.sh; редактор — python .claude/tools/play_scenario.py <ДЕРЕВО> <.scn>.
- долгое — run_in_background + python .claude/tools/studio.py wait <output-файл>; sleep ≤ {limits.MAX_SLEEP} с; ход в ожидании уведомления не заканчивай.
- лимиты (limits.py): вызовов {soft} (+{hard - soft} только на финальный шаг), отчёт ≤ {limits.REPORT_MAX_LINES} непустых строк — подробности в файл.""")


def lane_of(agent_type):
    return (limits.FEATURE_CALLS, limits.FEATURE_READ_LINES) if agent_type in FEATURE_LANE else \
           (limits.FIX_CALLS, limits.FIX_READ_LINES)


def code_of(prompt):
    m = re.search(r"You are agent (" + CODE_RE + r")", prompt or "")
    return m.group(1) if m else None


def base_code(code):
    return re.sub(r"(-(REV|FIX)\d*)?(-\d+)?$", "", code or "")


# ---- state and log --------------------------------------------------------------------------------------------------
def log(data, agent, decision, note=""):
    if DRY:
        return
    try:
        os.makedirs(limits.STUDIO_HOME, exist_ok=True)
        if os.path.exists(limits.GUARD_LOG) and os.path.getsize(limits.GUARD_LOG) > 20 * 1024 * 1024:
            os.replace(limits.GUARD_LOG, limits.GUARD_LOG + ".1")
        tin = data.get("tool_input") or {}
        with open(limits.GUARD_LOG, "a", encoding="utf-8") as f:
            f.write(json.dumps({"t": time.time(), "agent": agent, "type": data.get("agent_type"),
                                "tool": data.get("tool_name"), "decision": decision, "note": note[:200],
                                "cmd": str(tin.get("command") or tin.get("file_path") or tin.get("pattern") or "")
                                [:160 if decision == "deny" else 100]}) + "\n")
    except OSError:
        pass


def load_state(agent):
    path = os.path.join(limits.AGENT_STATE, f"{agent}.json")
    if DRY:
        return {"calls": 0}, path
    try:
        with open(path) as f:
            return json.load(f), path
    except (OSError, ValueError):
        return {"calls": 0}, path


def save_state(state, path):
    if DRY:
        return
    try:
        os.makedirs(limits.AGENT_STATE, exist_ok=True)
        with open(path, "w") as f:
            json.dump(state, f)
    except OSError:
        pass


def map_digest(lane, map_path=None, tree=None):
    """CODEMAP's index (every '## ' heading with its line number) and its hand-written Notes section."""
    map_path = map_path or os.path.join(limits.REPO, ".claude", "CODEMAP.md")
    try:
        lines = open(map_path, encoding="utf-8").read().split("\n")
    except OSError:
        return "[agent_guard] .claude/CODEMAP.md is missing — tell the lead; do not search the tree instead.\n" + cheat_sheet(lane)
    index = [f"{i + 1}: {l}" for i, l in enumerate(lines) if l.startswith("## ")]
    if tree:
        return (f"[agent_guard] КАРТА ТВОЕЙ ВЕТКИ (дерево {tree}): {map_path} — в ней и новые папки задачи. Читай СВОЙ "
                f"раздел диапазоном. Разделы (строка: заголовок):\n" + "\n".join(index))
    notes, inside = [], False
    for l in lines:
        if l.startswith("## "):
            inside = l.startswith("## Notes")
        elif inside and l.strip():
            notes.append(l)
    return ("[agent_guard] КАРТА ПРОЕКТА (.claude/CODEMAP.md). Разделы (строка: заголовок):\n" + "\n".join(index) +
            "\nЗаметки карты:\n" + "\n".join(notes[:40]) +
            "\nПрочитай СВОЙ раздел диапазоном (sed -n 'A,Bp' .claude/CODEMAP.md) — до этого чтение кода отклоняется. "
            "Поиск по многим файлам — Agent(subagent_type \"Explore\", model \"haiku\").\n" + cheat_sheet(lane))


# ---- lead rules -----------------------------------------------------------------------------------------------------
def lead_waste_refusal(cmd):
    """Owner 11-25: a merge cut by `timeout` left a half-written tree, and whole diffs were read into the lead."""
    if GIT_TREE_WRITE.search(cmd):
        return "`timeout` вокруг git, который пишет дерево, обрывает его на середине. Запусти без timeout, с run_in_background."
    big = [int(n) for n in re.findall(r"\bhead\s+-c\s*(\d+)", cmd) if int(n) > limits.LEAD_OUTPUT_CAP]
    if big:
        return (f"вывод {big[0]} знаков в контекст лида (> limits.LEAD_OUTPUT_CAP = {limits.LEAD_OUTPUT_CAP}) перечитывается "
                "каждым следующим запросом. Выжимка скриптом или агент reviewer.")
    if any(GIT_TEXT.search(part) and not CAPPED.search(part) for part in re.split(r"&&|;|\n", cmd)):
        return "дифф целиком в контекст лида не читается: --stat, выжимка (| head -60) или агент reviewer."
    return None


def target_tree(cmd, cwd):
    """The tree a git command acts on: `git -C <dir>`, else the last `cd <dir>` before it, else the hook's cwd."""
    m = re.search(r"\bgit\s+-C\s+(\"[^\"]+\"|'[^']+'|\S+)", cmd)
    if m:
        return m.group(1).strip("\"'")
    cds = re.findall(r"(?:^|[;&|(]\s*)cd\s+(\"[^\"]+\"|'[^']+'|\S+)", cmd)
    return cds[-1].strip("\"'") if cds else (cwd or ".")


def push_refusal(cmd, cwd):
    """Trunk-based (owner 10-22): no int/* branches; a task/* branch a day old and far behind dev is not pushed."""
    m = GIT_PUSH.search(cmd)
    if not m:
        return None
    args = m.group(1)
    if re.search(r"(?:^|[\s:])int/", args):
        return "int/* не создаём (trunk-based): принятая ветка вливается в dev через scripts/Dev/land_dev.sh."
    task = re.search(r"(?:^|[\s:])(task/[\w.\-]+)", args)
    if not task:
        return None
    tree = target_tree(cmd[:m.end()], cwd)
    rc, base = P.git(tree, "merge-base", limits.TRUNK, "HEAD")
    if rc or not base:
        return None
    try:
        born = int(P.git(tree, "log", "-1", "--format=%ct", base)[1] or 0)
        behind = int(P.git(tree, "rev-list", "--count", base + ".." + limits.TRUNK)[1] or 0)
    except ValueError:
        return None
    if born and time.time() - born > limits.STALE_BRANCH_H * 3600 and behind > limits.MAX_BEHIND:
        return (f"{task.group(1)} отрезана от dev {int((time.time() - born) / 3600)} ч назад и отстала на {behind} коммитов "
                "(ветка живёт день). Сначала scripts/Dev/merge_dev.sh, потом push.")
    return None


def stale_branch_refusal(code, prompt):
    """Owner 10-29: no new step on a branch whose oldest commit not in dev is older than limits.STALE_BRANCH_H."""
    if "ПОСАДКА:" in prompt:
        return None
    for br in (f"origin/task/{code}", f"origin/task/{base_code(code)}"):
        rc, out = P.git(limits.LAND_TREE, "log", "--format=%ct", "--reverse", f"{limits.TRUNK}..{br}")
        if rc == 0 and out:
            age = (time.time() - int(out.split()[0])) / 3600
            if age > limits.STALE_BRANCH_H:
                return (f"{br} не в dev уже {age:.0f} ч: новый шаг в невлитую ветку запрещён. Сначала влей её; работа на "
                        "посадку (слияние, ошибки компиляции) — с «ПОСАДКА: <что>» в промпте.")
            return None
    return None


def unbuilt_refusal(s, code):
    """Owner 11-25: one code run and one review-fix run may precede the build; the third waits for it."""
    since = 0
    try:
        with open(limits.BUILD_LEDGER, encoding="utf-8") as f:
            since = max((json.loads(line).get("t", 0) for line in f if line.strip()), default=0)
    except (OSError, ValueError):
        pass
    runs = [r for r in s["runs"].values() if r.get("t1", 0) > since and base_code(r.get("code")) == base_code(code)
            and r.get("type") not in READ_ONLY_AGENTS]
    if len(runs) < limits.RUNS_BEFORE_BUILD:
        return None
    return (f"{base_code(code)}: сдано {len(runs)} захода после последней сборки (limits.RUNS_BEFORE_BUILD) — следующий только "
            "после сборки пачки: studio.py merge, build_land.sh.")


def fix_brief_refusal(path, text):
    body = P.section(text, "Сделать")
    if body is None:
        return f"в брифе {path} нет раздела «## Сделать»."
    scen = P.section(text, "Сценарий")
    if not scen or not re.search(r"^\s*(Тогда|Then)[^\n]*\[[\w.-]+\]", scen, re.M):
        return f"в брифе {path} нет «## Сценарий» со строкой «Тогда [имя_кадра]: что видно» (спека = сценарий до кода)."
    arch = P.section(text, "Архитектура")
    missing = [k for k in ARCH_KEYS if not arch or not re.search(rf"^\s*[-*]?\s*{k}:\s*\S", arch, re.M)]
    if missing:
        return (f"в брифе {path} нет «## Архитектура» со строками {', '.join(k + ':' for k in ARCH_KEYS)} (не хватает: "
                f"{', '.join(missing)}): какой паттерн C++ взят за эталон, какие варианты отброшены, почему этот.")
    items, (max_items, max_chars) = re.findall(r"^\s*\d+[.)]\s", body, re.M), limits.FIX_BRIEF
    if len(items) > max_items or len(body) > max_chars:
        return (f"задача крупнее починки — «## Сделать» {len(items)} пунктов / {len(body)} знаков (limits.FIX_BRIEF = "
                f"{max_items} / {max_chars}). Это фича: architect → studio.py plan → engineer на пакет.")
    return None


def feature_brief_refusal(path, text, code, s):
    """An engineer runs ONE planned package of a design that is a parallel plan over disjoint files."""
    design = re.search(r"^## Дизайн[^\n]*\n\s*(\S+\.md)", text, re.M)
    if not design or not os.path.isfile(design.group(1)):
        return f"бриф {path} для engineer без «## Дизайн» с путём к файлу дизайна (агент architect)."
    errs = P.design_errors(design.group(1))
    if errs:
        return f"дизайн {design.group(1)} не годится к запуску: " + "; ".join(errs[:3])
    named = re.search(r"^Код:\s*(" + CODE_RE + r")\s*$", P.section(text, "Пакет") or "", re.M)
    if not named or named.group(1) != code:
        return (f"бриф {path}: в «## Пакет» нет строки «Код: {code}» — engineer запускается только на пакет, а бриф пакета "
                "пишет `python .claude/tools/studio.py plan <КОД> <дизайн.md>` (он же создаёт дерево и коммит контракта).")
    feat, letter = P.split_code(s, code)
    if not letter:
        return f"пакет {code} не спланирован: python .claude/tools/studio.py plan <КОД> <дизайн.md>, запуск — по его строкам."
    clash = P.overlap_refusal(s, feat, letter)
    if clash:
        return clash
    body = P.section(text, "Сделать") or ""
    items, (max_items, max_chars) = re.findall(r"^\s*\d+[.)]\s", body, re.M), limits.FEATURE_BRIEF
    if len(items) > max_items or len(body) > max_chars:
        return f"«## Сделать» {len(items)} пунктов / {len(body)} знаков (limits.FEATURE_BRIEF) — подробности живут в дизайне."
    return None


def free_gb():
    root = limits.WORKTREE_ROOT if os.path.isdir(limits.WORKTREE_ROOT) else os.path.splitdrive(limits.WORKTREE_ROOT)[0] + "/"
    try:
        return shutil.disk_usage(root or "/").free / 1024 ** 3
    except OSError:
        return float(limits.MIN_FREE_DISK_GB)


def lead_launch_refusal(tin, s, disk_gb=None):
    kind = (tin.get("subagent_type") or "general-purpose").lower()
    prompt = str(tin.get("prompt") or "")
    if "sonnet" in str(tin.get("model") or "").lower():
        return "sonnet не запускать (владелец 09-29: на задачу он дороже основной модели). Разведка — haiku Explore."
    if kind in P.NOT_COUNTED:
        return None
    code = code_of(prompt)
    if code and kind == "general-purpose":
        return ("одна правка идёт агентом fixer (SPEND-TOTAL-5X: у general-purpose полный набор инструментов, +22k на каждый запрос) — "
                'Agent(subagent_type: "fixer", …).')
    if not code:
        return "в промпте нет кода: первая строка «You are agent <КОД>» (для пакета фичи — <КОД>-<A..D>)."
    gb = free_gb() if disk_gb is None else disk_gb
    if gb < limits.MIN_FREE_DISK_GB:
        return (f"на диске деревьев ({limits.WORKTREE_ROOT}) свободно {gb:.1f} ГБ < limits.MIN_FREE_DISK_GB "
                f"({limits.MIN_FREE_DISK_GB}). Сначала studio.py landed для севших фич.")
    live = P.live_runs(s)
    cap = limits.NIGHT_MAX_AGENTS if P.night(s) else limits.MAX_AGENTS
    if len(live) >= cap:
        return (f"живых агентов {len(live)} — потолок владельца {cap}{' (ночной режим)' if P.night(s) else ''}: "
                f"{', '.join(sorted(r['code'] for r in live.values()))}. Дождись сдачи.")
    twin = [a for a, r in live.items() if r["code"] == code]
    if twin:
        return f"заход {code} ещё идёт ({twin[0]}): второй агент с тем же кодом правит то же дерево. Параллельная работа — другой пакет."
    if kind in READ_ONLY_AGENTS:
        return None
    if tin.get("isolation"):
        return "isolation не задаётся: дерево пакета создаёт studio.py plan (от коммита контракта, на диске limits.WORKTREE_ROOT)."
    found = BRIEF_PATH.search(prompt)
    if not found:
        return "рабочий агент запускается только по файлу брифа (…BRIEF*.md в промпте)."
    try:
        text = open(found.group(0), encoding="utf-8").read()
    except OSError:
        return f"бриф {found.group(0)} не читается."
    stall = (s.get("stalls") or {}).get(code, {}).get("n", 0)
    if stall >= limits.STALL_PASSES and "## Пересмотр" not in text:
        return (f"{code}: {stall} сдачи без продвижения — тем же путём не запускать: впиши в бриф «## Пересмотр» "
                "(причина застоя и новый подход).")
    # Owner 11-26 «вернуться к 15 %»: NET-PAWN-TELEPORT-FRAME burned seven fix passes on guesses. After two failed
    # fixes the next one is written from an architect's review of the whole path, not from the last symptom.
    fix_n = re.search(r"BRIEF_FIX(\d+)\.md$", found.group(0))
    review = os.path.join(os.path.dirname(found.group(0)), "DESIGN_REVIEW.md")
    if fix_n and int(fix_n.group(1)) > limits.FIXES_BEFORE_REVIEW and not os.path.exists(review):
        return (f"{code}: починка {fix_n.group(1)} без разбора — после {limits.FIXES_BEFORE_REVIEW} неудачных починок "
                f"сначала Agent(architect) пишет {review} (причина file:line + дешёвая проверка), бриф — из него.")
    why = feature_brief_refusal(found.group(0), text, code, s) if kind == "engineer" else fix_brief_refusal(found.group(0), text)
    return why or unbuilt_refusal(s, code) or stale_branch_refusal(code, prompt)


def lead_pre(data, s):
    tool, tin = data.get("tool_name") or "", data.get("tool_input") or {}
    if tool in ("Bash", "PowerShell"):
        cmd = str(tin.get("command") or "")
        return lead_waste_refusal(cmd) or push_refusal(cmd, data.get("cwd") or "")
    if tool == "Agent":
        return lead_launch_refusal(tin, s())
    if tool.startswith("mcp__notion__") or (tool == "ToolSearch" and "notion" in str(tin.get("query") or "").lower()):
        return "Notion читает и пишет только субагент board: Agent(subagent_type: \"board\", prompt: что прочитать или записать)."
    if tool == "SendMessage":
        target = str(tin.get("to") or tin.get("recipient") or tin.get("agent_id") or "").strip()
        ids = [target] + [a for a, r in s()["runs"].items() if r.get("code") == target]
        calls = max([load_state(a)[0].get("calls", 0) for a in ids if re.fullmatch(r"[\w.-]+", a)] or [0])
        if calls > limits.CONTINUE_MAX:
            return (f"агент {target} уже сделал {calls} вызовов (> limits.CONTINUE_MAX = {limits.CONTINUE_MAX}) — не "
                    "продолжать: свежий агент по «## NEXT» его REMAINDER.")
    return None


def lead_post(data):
    """Launch and resume are registered here, by the hook: the lead writes no event by hand."""
    tool, tin = data.get("tool_name") or "", data.get("tool_input") or {}
    resp = json.dumps(data.get("tool_response"), ensure_ascii=False)
    if tool == "Agent":
        kind, code, m = (tin.get("subagent_type") or "general-purpose").lower(), code_of(str(tin.get("prompt") or "")), AGENT_ID.search(resp)
        if m and code and kind not in P.NOT_COUNTED:
            with P.locked() as s:
                P.register_launch(s, m.group(1), code, kind)
            P.event("launch", code, m.group(1), kind)
    elif tool == "SendMessage":
        m = RESUMED_ID.search(resp)
        if m:
            with P.locked() as s:
                P.resume(s, m.group(1))


# ---- sub-agent rules ------------------------------------------------------------------------------------------------
def runs_editor(cmd):
    """True only when a shell segment EXECUTES the editor/runtime binary (not `cp Editor.exe`, not a path argument)."""
    for segment in re.split(r"[;&|\n]+|\$\(|`", cmd):
        words = segment.strip().split()
        while words and (re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", words[0]) or words[0] in ("exec", "time", "nohup", "env", "command")):
            words = words[1:]
        if words and words[0] in ("python", "python3", "py") and len(words) > 1 and "run_capped" in words[1]:
            continue
        if words and EDITOR_RUN.search(words[0].strip("\"'")):
            return True
    return False


def staged_claude_files(cmd, cwd):
    """True when the commit about to run would carry files under .claude/ (staged, or `-a` on modified ones)."""
    where = target_tree(cmd, cwd)
    _, staged = P.git(where, "diff", "--name-only", "--cached")
    if re.search(r"\bcommit\b[^;&|]*\s-[A-Za-z]*a", cmd):
        staged += "\n" + P.git(where, "diff", "--name-only")[1]
    return any(f.startswith(".claude/") for f in staged.split())


def script_rule(cmd, cwd):
    """A refusal text when a routine step bypasses its scripts/Dev script, else None."""
    tree = target_tree(cmd, cwd)
    has = lambda name: os.path.exists(os.path.join(tree, "scripts", "Dev", name))
    if GIT_PUSH.search(cmd) and "--delete" not in cmd and has("handoff_check.sh"):
        _, head = P.git(tree, "rev-parse", "HEAD")
        subject = "wip" if WIP_COMMIT.search(cmd) else P.git(tree, "log", "-1", "--format=%s")[1]
        if head and not subject.lower().startswith("wip") and not os.path.exists(os.path.join(tree, ".cache", "handoff", head + ".ok")):
            return ("Push без проверки: нет .cache/handoff/<HEAD>.ok. Сдача агента — коммит с темой «wip: <КОД> код готов» "
                    "(пушится без маркера, лид знает, что ветка ждёт сборки пачки).")
    if DEV_MERGE.search(cmd) and "merge_dev.sh" not in cmd and has("merge_dev.sh"):
        return "dev вливается только scripts/Dev/merge_dev.sh: он останавливается на конфликте и ловит расхождение .claude."
    if MIGRATOR_RUN.search(cmd) and "DESERT_MIGRATE_VIA_SCRIPT=1" not in cmd and "migrate.sh" not in cmd and has("migrate.sh"):
        return "SceneMigrator — только через scripts/Dev/migrate.sh [--write] <пути>: он отказывается от протухшего бинарника."
    if TEST_LOOP.search(cmd):
        return "Тесты выключены владельцем (CLAUDE.md «Build and run»): агент их не пишет и не гоняет."
    return None


def worker_pre(data, agent, agent_type, state, s):
    """The refusal text for a worker's call, or None. Mutates @p state (saved by the caller)."""
    (warn, soft, hard), read_lines = lane_of(agent_type)
    tool, tin = data.get("tool_name", ""), data.get("tool_input") or {}
    cmd = tin.get("command", "") if tool == "Bash" else ""
    if not UNCOUNTED_WAIT.match(cmd):
        state["calls"] = state.get("calls", 0) + 1
    calls = state.get("calls", 0)

    if soft < calls <= hard:  # the finishing window: scenario, frames, commit, push, REMAINDER
        same = bool(cmd) and state.get("finish_last") == cmd
        state["finish_rep"] = state.get("finish_rep", 1) + 1 if same else 1
        state["finish_last"] = cmd
        if tool in ("Edit", "MultiEdit", "NotebookEdit", "Grep", "Glob", "Agent") or state["finish_rep"] >= 3:
            return (f"Вызов {calls}: после {soft} — только финальный шаг до {hard} (сценарий, кадры, коммит, пуш, REMAINDER); "
                    "правки, поиск и одна команда трижды подряд запрещены.")
    if calls > hard and not (tool in ("SendMessage", "Write") or (tool == "Bash" and ALWAYS_ALLOWED_AFTER_LIMIT.match(cmd))):
        return (f"Лимит {hard} вызовов исчерпан ({calls}). Закоммить и запушь сделанное, запиши REMAINDER.md (файл:строка) "
                "и отчитайся; остаток уйдёт свежему агенту.")

    if tool == "Agent":
        kind = (tin.get("subagent_type") or "general-purpose").lower()
        child = code_of(str(tin.get("prompt") or ""))
        chain = (kind == "reviewer" and agent_type in P.WRITERS and child.endswith("-REV")) or \
                (kind == "acceptance" and agent_type == "lander")
        if chain:  # owner 11-27: the worker drives its own chain, the lead wakes once (limits.CHAIN_REVIEWS)
            if tin.get("run_in_background"):
                return "Свой reviewer/acceptance — на переднем плане (без run_in_background): иначе уведомление будит лида."
            state["reviews"] = state.get("reviews", 0) + 1
            if state["reviews"] > limits.CHAIN_REVIEWS:
                return f"Больше limits.CHAIN_REVIEWS ({limits.CHAIN_REVIEWS}) ревью за заход: сдай как есть, accept stuck."
            return None
        if kind != "explore":
            return ("Рабочий помощник запрещён: разрешены Explore на haiku и свой reviewer («You are agent <КОД>-REV», на переднем плане). "
                    "Не влезает — коммит, пуш, REMAINDER.")
        if (tin.get("model") or "").lower() != "haiku":
            return "Explore запускается на haiku: Agent(subagent_type: \"Explore\", model: \"haiku\", ...)."
        state["explored"] = True
    if tool == "Bash" and BUILD_RM.search(cmd):
        return "Дерево сборки не удаляется: rm -rf build стоит полной пересборки пачки. Сборка — у лида."
    if tool == "Bash" and ASSET_RM.search(cmd):
        return ("Ассеты проекта (Content/, .demat/.stmesh/.detex/.desce/…) из шелла не удаляются: через редактор (проверка "
                "ссылок) или строкой в REMAINDER — удаляет лид.")

    if (tool == "Bash" and "CODEMAP.md" in cmd and re.search(r"sed\s+-n\s+'?\d+,\d+p", cmd)) or \
            (tool == "Read" and (tin.get("file_path") or "").endswith("CODEMAP.md") and tin.get("offset")):
        state["map_read"] = True
    brief = BRIEF_PATH.search(cmd if tool == "Bash" else (tin.get("file_path") or ""))
    if brief and not state.get("map_read"):
        try:
            text = open(brief.group(0), encoding="utf-8").read()
            if "## Карта" in text or "## Пакет" in text:  # the brief names the files: that is the map of this run
                state["map_read"] = True
        except OSError:
            pass
    shell = HEREDOC_BODY.sub("", cmd)  # a heredoc body is text being written, not a command that reads code
    reading_code = (tool == "Read" and CODE_FILE.search(tin.get("file_path") or "")) or \
                   (tool == "Bash" and CODE_READ.search(shell) and CODE_FILE.search(shell))
    if reading_code and not state.get("map_read") and not state.get("explored"):
        return ("Сначала карта: прочитай свой раздел .claude/CODEMAP.md диапазоном (или бриф пакета со строкой «Файлы:»), "
                "либо спроси Explore на haiku — и только потом читай код.")
    if tool == "Read" and CODE_FILE.search(tin.get("file_path") or "") and \
            (not tin.get("limit") or int(tin.get("limit") or 0) > read_lines):
        return f"Код читается диапазоном ≤ {read_lines} строк: Read(file_path, offset, limit≤{read_lines}) или sed -n 'A,Bp'."
    if tool == "Bash" and CODE_FILE.search(cmd):
        if re.search(r"(^|[;&|(]\s*)cat\s+(?!>)[^|;&>]*\.(cpp|hpp|h|glslh|shader|lua|mm)\b", cmd):
            return f"`cat` исходника целиком запрещён: grep -n → sed -n 'A,Bp' (≤ {read_lines} строк)."
        for a, b in re.findall(r"sed\s+-n\s+['\"]?(\d+),(\d+)p", cmd):
            if int(b) - int(a) > read_lines:
                return f"sed -n {a},{b}p — {int(b) - int(a)} строк > {read_lines}. Читай уже."

    if tool in ("Edit", "Write", "MultiEdit", "NotebookEdit"):
        path = tin.get("file_path") or ""
        if "/.claude/" in path.replace("\\", "/"):
            return "Файлы .claude/ (хуки, навыки, агенты) правит только лид."
        owned = P.edit_refusal(s(), path)
        if owned:
            return owned
    if tool != "Bash":
        return None

    denial = script_rule(cmd, data.get("cwd") or "") or push_refusal(cmd, data.get("cwd") or "")
    if denial:
        return denial
    if GIT_COMMIT.search(cmd) and staged_claude_files(cmd, data.get("cwd")):
        return "В коммите есть файлы .claude/ — их коммитит только лид: git restore --staged .claude && git checkout -- .claude"
    if any(rx.search(shell) for rx in TREE_SEARCH) and not LOG_SEARCH.search(shell):
        return ("Поиск по дереву в своём контексте запрещён. Где определено имя — scripts/Dev/sym.sh <Имя>, где используется — "
                "sym.sh --refs <Имя>; шире — Agent(subagent_type: \"Explore\", model: \"haiku\"). Сам ищи только в известных "
                "файлах: grep -n <шаблон> <файл>.")
    if FIND.search(shell) and "-maxdepth" not in shell:
        return "`find` без -maxdepth — это поиск по дереву; Explore или `find <папка> -maxdepth 2 ...`."
    for m in SLEEP.finditer(cmd):
        if int(m.group(1)) > limits.MAX_SLEEP:
            return (f"sleep {m.group(1)} > {limits.MAX_SLEEP} с: кэш истекает через 5 минут и весь контекст пишется заново. "
                    "Жди кусками: python .claude/tools/studio.py wait <файл> [--marker ТЕКСТ].")
    if not tin.get("run_in_background") and int(tin.get("timeout") or 0) > limits.MAX_SLEEP * 1000 + 10000:
        return (f"timeout {int(tin['timeout']) // 1000} с > {limits.MAX_SLEEP + 10} с: вызов дольше 5 минут сбрасывает кэш. "
                "Запусти с run_in_background: true и жди python .claude/tools/studio.py wait <output-файл>.")
    if LEAD_ONLY_RUNS.search(cmd):
        return "CheckTidy, CheckGluedText и handoff гоняет только лид."
    if CI_WAIT.search(cmd):
        return "CI не ждёшь: запушь, назови id прогона в отчёте (gh run list --branch <ветка> --limit 1, один раз) и заканчивай."
    if re.search(r"DESERT_EDITOR_MANUAL|DESERT_SCENARIO\s*=", cmd) or (runs_editor(cmd) and "run_capped" not in cmd):
        return ("Живой редактор агент поднимает только python .claude/tools/play_scenario.py <ДЕРЕВО> <Scenarios/…/КОД.scn> "
                "[--hold]: доказательство = сценарий, один редактор за раз.")
    return None


def worker_post(data, agent, agent_type, state, path):
    """additionalContext for a worker after a call, or None."""
    lane, _ = lane_of(agent_type)
    warn, soft, hard = lane
    if not state.get("map_shown"):
        state["map_shown"] = True
        save_state(state, path)
        return map_digest(lane)
    pin = data.get("tool_input") or {}
    tm = WORKTREE.search(pin.get("command", "") or pin.get("file_path", "") or "")
    tree = tm.group(1).replace("\\", "/") if tm else None
    if tree and os.name == "nt" and re.match(r"^/[a-z]/", tree):
        tree = tree[1].upper() + ":" + tree[2:]
    committed = data.get("tool_name") == "Bash" and GIT_COMMIT.search(pin.get("command", "") or "")
    if tree and os.path.isdir(tree) and (not state.get("branch_map") or committed):
        first = not state.get("branch_map")
        out = os.path.join(limits.STUDIO_HOME, "maps", agent + "-CODEMAP.md")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen_codemap.py")
        try:
            subprocess.run([sys.executable, gen], cwd=tree, env=dict(os.environ, CODEMAP_OUT=out), capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            pass
        state["branch_map"] = out
        save_state(state, path)
        if os.path.exists(out):
            return map_digest(lane, out, tree) if first else "[agent_guard] Карта дерева пересобрана после коммита: " + out
    if data.get("tool_name") == "Bash" and pin.get("run_in_background"):
        m = re.search(r"Output is being written to: (\S+)", json.dumps(data.get("tool_response", "")))
        return ("[agent_guard] Задача в фоне. НЕ заканчивай ход в ожидании уведомления: простой > 5 мин сбрасывает кэш. Жди: "
                "python .claude/tools/studio.py wait " + (m.group(1).rstrip("\\.\"") if m else "<output-файл>") + " [--marker ТЕКСТ].")
    calls = state.get("calls", 0)
    if calls >= warn:
        return (f"[agent_guard] Вызов {calls}/{soft} (+{hard - soft} только на финальный шаг). Доделай ТЕКУЩИЙ шаг, новый крупный "
                "не начинай; закоммить, запушь, отчитайся. Остаток — REMAINDER.md (файл:строка, что на что, что проверено): "
                "продлений нет.")
    return None


# ---- stop -----------------------------------------------------------------------------------------------------------
def last_report_text(data):
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
                if isinstance(m, dict) and m.get("role") == "assistant" and isinstance(m.get("content"), list):
                    parts = [c.get("text", "") for c in m["content"] if isinstance(c, dict) and c.get("type") == "text"]
                    if any(t.strip() for t in parts):
                        last = "\n".join(parts)
        if last:
            return last
    return ""


def subagent_stop(data):
    """("block", reason) for a report that must be rewritten, else None. The run is marked handed in either way."""
    kind = (data.get("agent_type") or "").lower()
    named = re.search(r"agent-([0-9a-f]{8,})\.jsonl$", str(data.get("agent_transcript_path") or ""))
    aid = named.group(1) if named else (data.get("agent_id") or "")
    if kind == "explore" or (named and data.get("agent_id") and named.group(1) != data.get("agent_id")):
        return None  # a discovery agent, or a worker's own child: nothing is handed to the lead
    text = last_report_text(data)
    if not data.get("stop_hook_active"):
        lines = [l for l in text.splitlines() if l.strip()]
        if len(lines) > limits.REPORT_MAX_LINES:
            log(data, aid or "?", "block", f"report {len(lines)} lines")
            return ("block", f"[agent_guard] Отчёт {len(lines)} непустых строк, предел limits.REPORT_MAX_LINES = "
                    f"{limits.REPORT_MAX_LINES}. Подробности запиши в файл и перепиши отчёт: итог, цифры, путь, остаток.")
        design = DESIGN_PATH.search(text) if kind == "architect" else None
        if design and os.path.isfile(design.group(0)):
            errs = P.design_errors(design.group(0))
            if errs:
                log(data, aid or "?", "block", "design: " + errs[0])
                return ("block", f"[agent_guard] Дизайн {design.group(0)} не планируется (studio.py plan откажет):\n- " +
                        "\n- ".join(errs) + "\nИсправь «## Порядок»: «### Контракт» + 1.." + str(limits.MAX_PACKAGES) +
                        " «### Пакет A — имя» со строками «Файлы:» и «Закрывает:», пакеты параллельны и не пересекаются по файлам;"
                        " каждое отличие закрыто одним пакетом, файл сценария из «## Приёмка» — в «Файлы:» одного пакета.")
    if aid and kind not in P.NOT_COUNTED and not DRY:
        with P.locked() as s:
            P.mark_stopped(s, aid, kind, code_of(text) or "")
    return None


# ---- dispatch -------------------------------------------------------------------------------------------------------
def decide(data, s=None):
    """(kind, text) with kind in deny | block | context, or None. @p s: a pipeline state to judge against (self-check)."""
    cache = {}

    def state_now():
        if "s" not in cache:
            cache["s"] = s if s is not None else P.load()
        return cache["s"]

    event = data.get("hook_event_name") or "PreToolUse"
    agent, agent_type = data.get("agent_id"), (data.get("agent_type") or "").lower()
    if event == "SubagentStop":
        return subagent_stop(data)
    if event == "SubagentStart":
        if not agent or agent_type in P.NOT_COUNTED:
            return None
        state, path = load_state(agent)
        state["map_shown"] = True
        save_state(state, path)
        return ("context", map_digest(lane_of(agent_type)[0]))
    if not agent:  # the lead's own session
        if event == "PreToolUse":
            why = lead_pre(data, state_now)
            if why:
                log(data, "lead", "deny", why)
                return ("deny", "[agent_guard] Лид: " + why)
        elif event == "PostToolUse" and not DRY:
            lead_post(data)
        return None
    if agent_type == "board":
        name = str(data.get("tool_name") or "")
        if event == "PreToolUse" and name not in ("ToolSearch", "Read") and not name.startswith("mcp__notion__"):
            log(data, agent, "deny", "board tool")
            return ("deny", f"[agent_guard] Агент board работает только с Notion, вызов {name} запрещён.")
        return None
    if agent_type == "explore":
        return None
    state, path = load_state(agent)
    if event == "PostToolUse":
        text = worker_post(data, agent, agent_type, state, path)
        return ("context", text) if text else None
    why = worker_pre(data, agent, agent_type, state, state_now)
    save_state(state, path)
    log(data, agent, "deny" if why else "allow", why or "")
    return ("deny", "[agent_guard] " + why) if why else None


def emit(event, verdict):
    if not verdict:
        return
    kind, text = verdict
    if kind == "deny":
        out = {"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny", "permissionDecisionReason": text}}
    elif kind == "block":
        out = {"decision": "block", "reason": text}
    else:
        out = {"hookSpecificOutput": {"hookEventName": event, "additionalContext": text}}
    sys.stdout.write(json.dumps(out, ensure_ascii=False))


# ---- self-check -----------------------------------------------------------------------------------------------------
def self_check():
    """Replays known-bad calls through decide(); returns (names that bite, names that failed)."""
    global DRY
    DRY = True
    tmp = tempfile.mkdtemp(prefix="agent_guard_selfcheck_").replace("\\", "/")
    write = lambda name, text: (open(f"{tmp}/{name}", "w", encoding="utf-8").write(text), f"{tmp}/{name}")[1]
    sections = "## Эталон\nx\n## Отличия\n- **D1.** a — `A.cpp:1`\n- **D2.** b — `B.cpp:1`\n## Архитектура\nx\n"
    accept = "## Приёмка\nСценарий: Scenarios/F/F.scn\nТогда [f]: x\n"
    pa, pb = "### Пакет A — a\nФайлы: Desert/A/\nЗакрывает: D1\n", "### Пакет B — b\nФайлы: Desert/B/, Scenarios/F/F.scn\nЗакрывает: D2\n"
    good = write("GOOD-DESIGN.md", sections + "## Порядок\n" + pa + pb + accept)
    lost = write("LOST-DESIGN.md", sections + "## Порядок\n" + pa + pb.replace("Закрывает: D2\n", "Закрывает: D1\n") + accept)
    serial = write("SERIAL-DESIGN.md", sections + "## Порядок\n" + pa + "1. день 2: пакет B после пакета A\n" + pb + accept)
    clash = write("CLASH-DESIGN.md", sections + "## Порядок\n" + pa + pb.replace("Desert/B/", "Desert/A/X.cpp") + accept)
    pk = lambda files, state="planned": {"name": "x", "files": files, "tree": f"{tmp}/wt/F-", "state": state}
    s = {"v": 1, "night_until": 0, "board_queue": [], "stalls": {"STUCK": {"head": "", "n": 9}},
         "features": {"F": {"stage": "code", "t": time.time(), "contract": ["Desert/F/Api.hpp"], "base": "x",
                            "packages": {"A": dict(pk(["Desert/A/"]), tree=f"{tmp}/wt/F-A"),
                                         "B": dict(pk(["Desert/B/"]), tree=f"{tmp}/wt/F-B")}},
                      "G": {"stage": "code", "t": time.time(), "contract": [], "base": "x",
                            "packages": {"A": dict(pk(["Desert/A/Sub/"]), tree=f"{tmp}/wt/G-A")}}},
         "runs": {"live1": {"code": "F-A", "type": "engineer", "t0": time.time(), "t1": 0}}}
    serial_no = iter(range(1000))  # one file per case: two cases of one code must not share a brief
    brief = lambda code, design, extra="": write(f"{next(serial_no)}-{code}-BRIEF.md", f"## Дизайн\n{design}\n## Пакет\nКод: {code}\n## Сценарий\n"
                                                 f"Тогда [f]: x\n## Сделать\n1. a\n{extra}")
    fix = lambda name, do: write(name + "-BRIEF.md", "## Архитектура\nЭталон: a\nВарианты: b\nВыбор: c\n## Сценарий\nТогда [f]: x\n"
                                 f"## Сделать\n{do}")
    launch = lambda code, path, kind="engineer", **kw: {"tool_name": "Agent", "tool_input": dict(
        {"subagent_type": kind, "prompt": f"You are agent {code}. Your brief: {path}."}, **kw)}
    sub = lambda tool, **tin: {"tool_name": tool, "tool_input": tin, "agent_id": "selfcheck", "agent_type": "general-purpose"}
    bash = lambda cmd, **kw: sub("Bash", command=cmd, **kw)
    bad = {
        "overlapping file ownership": launch("G-A", brief("G-A", good)),
        "a serial plan over days": launch("F-B", brief("F-B", serial)),
        "packages of one design overlap": launch("F-B", brief("F-B", clash)),
        "a difference no package closes": launch("F-B", brief("F-B", lost)),
        "engineer without a planned package": launch("NEW-A", brief("NEW-A", good)),
        "engineer with isolation": launch("F-B", brief("F-B", good), isolation="worktree"),
        "edit of another package's file": sub("Edit", file_path=f"{tmp}/wt/F-B/Desert/A/X.cpp"),
        "edit of a contract header": sub("Write", file_path=f"{tmp}/wt/F-B/Desert/F/Api.hpp"),
        "second run with a live code": launch("F-A", brief("F-A", good)),
        "launch without a code": {"tool_name": "Agent", "tool_input": {"subagent_type": "reviewer", "prompt": "review it"}},
        "launch without a brief": {"tool_name": "Agent", "tool_input": {"subagent_type": "fixer", "prompt": "You are agent X-1. Do it."}},
        "fat fix brief": launch("X-2", fix("X-2", "1. a\n2. b\n3. c\n"), "fixer"),
        "fix brief without a scenario": launch("X-3", write("X-3-BRIEF.md", "## Сделать\n1. a\n"), "fixer"),
        "stalled code without a rethink": launch("STUCK", fix("STUCK", "1. a\n"), "fixer"),
        "sonnet": launch("X-4", fix("X-4", "1. a\n"), "fixer", model="sonnet"),
        "lead cuts a merge with timeout": {"tool_name": "Bash", "tool_input": {"command": "timeout 100 git merge --no-ff x"}},
        "lead reads a whole diff": {"tool_name": "Bash", "tool_input": {"command": "cd x && git diff -U2 a..b -- '*.cpp'"}},
        "lead prints 48k chars": {"tool_name": "Bash", "tool_input": {"command": "cat f | head -c 48000"}},
        "lead queries Notion": {"tool_name": "mcp__notion__notion-query-data-sources", "tool_input": {"query": "SELECT 1"}},
        "lead loads Notion schemas": {"tool_name": "ToolSearch", "tool_input": {"query": "select:mcp__notion__notion-fetch"}},
        "int branch push": {"tool_name": "Bash", "tool_input": {"command": "git push origin int/b9"}},
        "board agent runs Bash": {"tool_name": "Bash", "tool_input": {"command": "ls"}, "agent_id": "b", "agent_type": "board"},
        "tree search": bash("grep -rn x ."), "find without depth": bash("find . -name x"), "long sleep": bash("sleep 600"),
        "call longer than the cache": bash("scripts/Dev/x.sh", timeout=600000),
        "agent spawns a worker": sub("Agent", subagent_type="general-purpose", prompt="x"),
        "explore not on haiku": sub("Agent", subagent_type="Explore", prompt="x"),
        "agent removes assets": bash("rm Projects/Desert/Content/Materials/a.demat"),
        "agent removes the build tree": bash("rm -rf build"),
        "code before map": bash("grep -n Foo Desert/X.cpp"), "whole-file Read": sub("Read", file_path="/x/Desert/X.cpp"),
        "edit .claude": sub("Edit", file_path="/x/.claude/tools/agent_guard.py"),
        "agent runs tidy": bash("bash scripts/CI/CheckTidy.sh abc"), "agent runs tests": bash("bash scripts/MacOS/RunTests.sh"),
        "agent waits for CI": bash("gh run watch 1 --exit-status"),
        "CI poll loop": bash("for i in $(seq 20); do gh run view 1 --json status; sleep 10; done"),
        "editor outside a scenario": bash("cd Editor && ../build/Bin/Debug/Editor"),
        "editor manual flag": bash("DESERT_EDITOR_MANUAL=1 python .claude/tools/run_capped.py build/Bin/Debug/Editor.exe"),
    }
    legal = {
        "engineer on a planned package": launch("F-B", brief("F-B", good)),
        "small fix brief": launch("X-5", fix("X-5", "1. a\n"), "fixer"),
        "reviewer with a code": {"tool_name": "Agent", "tool_input": {"subagent_type": "reviewer", "prompt": "You are agent F-A-REV. Review."}},
        "lead launches the board agent": {"tool_name": "Agent", "tool_input": {"subagent_type": "board", "prompt": "tails"}},
        "board agent queries Notion": {"tool_name": "mcp__notion__notion-query-data-sources", "tool_input": {"query": "SELECT 1"},
                                       "agent_id": "b", "agent_type": "board"},
        "edit of an own file": sub("Edit", file_path=f"{tmp}/wt/F-B/Desert/B/Y.cpp"),
        "edit of an unowned file": sub("Edit", file_path=f"{tmp}/wt/F-B/Desert/New/Z.cpp"),
        "lead reads a diff stat": {"tool_name": "Bash", "tool_input": {"command": "git diff --shortstat a..b"}},
        "lead runs a merge": {"tool_name": "Bash", "tool_input": {"command": "git merge --no-ff x"}},
        "heredoc that mentions a header": bash("cat > notes.md <<'EOF'\nsee Desert/X.hpp\nEOF\ngrep -n x notes.md"),
        "worker waits with studio.py": bash("python .claude/tools/studio.py wait out.txt --marker DONE"),
    }
    failed = []
    real_free, globals()["free_gb"] = free_gb, lambda: 999.0  # the disk is not what is checked here
    try:
        for name, case in bad.items():
            verdict = decide(dict(case, hook_event_name="PreToolUse", cwd=tmp), s)
            if not verdict or verdict[0] != "deny":
                failed.append(name)
        for name, case in legal.items():
            verdict = decide(dict(case, hook_event_name="PreToolUse", cwd=tmp), s)
            if verdict:
                failed.append("REFUSED a legal call: " + name + " — " + verdict[1][:120])
        if not lead_launch_refusal(launch("F-B", brief("F-B", good))["tool_input"], s, disk_gb=1.0):
            failed.append("launch on a full disk")
        long_report = {"hook_event_name": "SubagentStop", "agent_type": "engineer", "agent_id": "x",
                       "last_assistant_message": "\n".join("line" for _ in range(limits.REPORT_MAX_LINES + 5))}
        if (subagent_stop(long_report) or ("",))[0] != "block":
            failed.append("long report")
        serial_stop = {"hook_event_name": "SubagentStop", "agent_type": "architect", "agent_id": "x",
                       "last_assistant_message": f"design: {serial}"}
        if (subagent_stop(serial_stop) or ("",))[0] != "block":
            failed.append("architect hands in a serial plan")
    finally:
        globals()["free_gb"] = real_free
        shutil.rmtree(tmp, ignore_errors=True)
        DRY = False
    return list(bad) + ["launch on a full disk", "long report", "architect hands in a serial plan"], failed


def main():
    if "--self-check" in sys.argv:
        names, failed = self_check()
        msg = (f"[agent_guard] self-check OK: {len(names)} known-bad calls refused ({', '.join(names)})." if not failed else
               f"[agent_guard] SELF-CHECK FAILED — these rules no longer bite: {'; '.join(failed)}. "
               "Restore .claude/tools/agent_guard.py from git history BEFORE launching any agent.")
        sys.stdout.write(json.dumps({"hookSpecificOutput": {"hookEventName": "SessionStart", "additionalContext": msg}},
                                    ensure_ascii=False))
        sys.exit(1 if failed and "--strict" in sys.argv else 0)
    try:
        data = json.load(sys.stdin)
    except ValueError:
        sys.exit(0)
    emit(data.get("hook_event_name") or "PreToolUse", decide(data))
    sys.exit(0)


if __name__ == "__main__":
    main()
