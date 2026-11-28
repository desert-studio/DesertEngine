#!/usr/bin/env python3
"""studio — the lead's one tool for the feature pipeline. Every command prints a digest of a few lines.

  studio.py digest                         the pipeline in <= 15 lines (also printed at every session start / compact)
  studio.py plan CODE DESIGN.md [--dry-run] check the design, commit the contract, make one tree and one brief per package
  studio.py launch CODE                    the Agent(...) lines for the packages still to launch
  studio.py accept CODE[-PKG] [done|wip|stuck|refused] [FRAME...]   close a run: spend, tree, ownership, frames, stall
  studio.py merge CODE [CODE...]           merge the accepted packages into the land tree (ownership re-proven, the
                                           scenario shoots every «Тогда [кадр]» of the design)
  studio.py built ok|fail [note]           the batch build's verdict for every feature at stage `build`
  studio.py passed CODE                    acceptance passed -> stage `land`
  studio.py landed CODE                    the branches are in dev: trees and branches removed, the design archived
                                           (limits.ARCHIVE), card queued as done with the Wiki line
  studio.py drop CODE "reason"             take a feature out of flight honestly
  studio.py board                          ONE batched request for the board agent (all queued card updates)
  studio.py night on [HH:MM] | off         night mode (ceiling and floor from limits)
  studio.py wait FILE [--marker TEXT]      block <= limits.WAIT_SLICE s on a background job's output file
  studio.py report                         spend and lead-time per tag
  studio.py migrate [--dry-run]            one-time move of the old ~/.claude state files into limits.STUDIO_HOME

Replaces ~/.claude/tools/accept.sh, ledger.py, process_event.py and .claude/tools/night_mode.sh.
Errors are values: a refused command prints the reason and exits 2; nothing is half-applied silently.
"""
import csv
import glob
import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import limits
import pipeline as P

for _stream in (sys.stdout, sys.stderr):
    _stream.reconfigure(encoding="utf-8")

HERE = os.path.dirname(os.path.abspath(__file__))
CARD_OF_STAGE = {"design": "В работе", "code": "В работе", "review": "На проверке", "build": "На проверке",
                 "accept": "На проверке", "land": "На проверке", "done": "Сделано", "dropped": "Заблокировано"}


def refuse(text):
    print("[studio] ОТКАЗ: " + text)
    sys.exit(2)


event = P.event


def free_gb():
    root = limits.WORKTREE_ROOT if os.path.isdir(limits.WORKTREE_ROOT) else os.path.dirname(limits.WORKTREE_ROOT) or "."
    try:
        return shutil.disk_usage(root).free / 1024 ** 3
    except OSError:
        return 0.0


# ---- plan -----------------------------------------------------------------------------------------------------------
def brief_text(code, letter, pk, design, contract, then, base):
    return "\n".join([
        f"# {code}-{letter} — {pk['name']}", "", "## Дизайн", design, "", "## Пакет",
        f"Код: {code}-{letter}", f"Дерево: {pk['tree']} (ветка {pk['branch']}, от коммита контракта {base[:9]})",
        "Файлы: " + ", ".join(pk["files"]),
        "Закрывает: " + ", ".join(pk.get("closes") or []),
        *(["Читает (только эти строки): " + ", ".join(f"{f}:" + ";".join(f"{a}-{b}" for a, b in r) for f, r in pk["reads"].items())]
          if pk.get("reads") else []),
        "Контракт (не править): " + (", ".join(contract) if contract else "нет общих заголовков"), "",
        "## Сценарий", *then, "", *SPEND(),
        "## Сделать",
        f"1. Раздел «### Пакет {letter}» из «## Порядок» дизайна целиком: в своём дереве, только в своих файлах.",
        f"2. Коммит «wip: {code}-{letter} код готов», push ветки; в отчёте — каждое отличие строки «Закрывает»: закрыто (file:line) или нет и почему; "
        "строки NEEDS/CONTRACT-BREAK, если были.", "", *CHAIN(f"{code}-{letter}", pk["branch"], pk["tree"], pk["brief"])])


def SPEND():
    """SPEND_RULES 11-27: the spend conditions travel inside every package brief."""
    return ["## Расход (.claude/SPEND_RULES.md)",
            f"- Читаешь не больше limits.PACKAGE_READ_CHARS ({limits.PACKAGE_READ_CHARS // 1000}k знаков) своих файлов; "
            "файлы длиннее 2000 строк — только диапазонами по grep, никогда целиком.",
            f"- Контекст {limits.CONTEXT_MAX[1] // 1000}k: остаётся limits.CONTEXT_FINISH_CALLS ({limits.CONTEXT_FINISH_CALLS}) "
            "вызовов — коммит, push, REMAINDER.md (файл:строка, «## NEXT»); дальше свежий агент.",
            "- Не собираешь, не ждёшь опросом, не запускаешь лишних агентов.", ""]


def CHAIN(code, branch, tree, brief):
    """Owner 11-27: the worker owns its task to acceptance; the lead wakes once, at its report."""
    return ["## Цепочка (ты ведёшь задачу до приёма; лид просыпается один раз — на твой отчёт)",
            "1. Код, коммит «wip: <КОД> код готов», push.",
            f'2. Agent(subagent_type: "reviewer", prompt: "You are agent {code}-REV. Review branch {branch} in {tree} against {brief}; '
            'ACCEPT or RETURN with at most 2 defects.") — на переднем плане, не run_in_background (limits.CHAIN_REVIEWS).',
            "3. RETURN или MINOR — чини в этом же заходе, коммит, push; второго ревью нет (владелец 11-27).",
            f"4. python .claude/tools/studio.py accept {code} done|stuck",
            "5. Отчёт: вердикт последнего ревью, коммит, что докажет только сборка пачки или кадр.", ""]


def launch_line(code, letter, pk):
    return (f'Agent(subagent_type: "engineer", run_in_background: true, description: "{code}-{letter}", '
            f'prompt: "You are agent {code}-{letter}. Your brief: {pk["brief"]}. Read it first; it is the whole task. You own the chain: code, your reviewer, your fixes, accept. '
            f'Tree: {pk["tree"]}.")')


def cmd_plan(args):
    dry = "--dry-run" in args
    args = [a for a in args if a != "--dry-run"]
    if len(args) != 2:
        refuse("studio.py plan CODE DESIGN.md [--dry-run]")
    code, design = args[0], os.path.abspath(args[1]).replace("\\", "/")
    errs = P.design_errors(design)
    if errs:
        refuse(f"дизайн {design} не планируется:\n  - " + "\n  - ".join(errs))
    d = P.parse_design(design)
    s = P.load()
    if code in P.in_flight(s):
        refuse(f"{code} уже в полёте (стадия {s['features'][code]['stage']}): studio.py digest")
    why = P.plan_refusal(s, code)
    if why:
        refuse(why)
    if free_gb() < limits.MIN_FREE_DISK_GB:
        refuse(f"на диске {limits.WORKTREE_ROOT} свободно {free_gb():.1f} ГБ < limits.MIN_FREE_DISK_GB")
    packages = {}
    for letter, p in sorted(d["packages"].items()):
        packages[letter] = {"name": p["name"], "files": p["files"], "closes": p["closes"], "reads": p.get("reads") or {}, "tree": f"{limits.WORKTREE_ROOT}/{code}-{letter}",
                            "branch": f"task/{code}-{letter}", "brief": f"{limits.BRIEFS}/{code}-{letter}-BRIEF.md",
                            "state": "planned", "head": "", "stall": 0, "strays": []}
    if dry:
        print(f"[studio] {code}: план годен — контракт {len(d['contract'])} файл(ов), пакетов {len(packages)}: " +
              "; ".join(f"{l} {p['name']} ({len(p['files'])} шаблонов)" for l, p in packages.items()))
        return
    land = limits.LAND_TREE
    P.git(land, "fetch", "origin", "dev", "--quiet")
    rc, base = P.git(land, "rev-parse", limits.TRUNK)
    if rc:
        refuse(f"нет {limits.TRUNK} в {land}: {base}")
    os.makedirs(limits.WORKTREE_ROOT, exist_ok=True)
    branch = f"task/{code}"
    if d["contract"]:
        ctree = f"{limits.WORKTREE_ROOT}/{code}-contract"
        rc, out = P.git(land, "worktree", "add", ctree, "-b", branch, limits.TRUNK)
        if rc:
            refuse(f"дерево контракта не создано: {out[-300:]}")
        for rel in d["contract"]:
            dst = os.path.join(ctree, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(os.path.join(P.contract_dir(design), rel), dst)
        P.git(ctree, "add", "--", *d["contract"])
        rc, out = P.git(ctree, "commit", "-m", f"contract: {code} - the shared headers every package builds against")
        if rc:
            refuse(f"коммит контракта не прошёл (заголовки совпадают с dev?): {out[-300:]}; дерево {ctree} оставлено")
        _, base = P.git(ctree, "rev-parse", "HEAD")
        P.git(land, "worktree", "remove", ctree)
    else:
        rc, out = P.git(land, "branch", branch, limits.TRUNK)
        if rc:
            refuse(f"ветка {branch} не создана: {out[-300:]}")
    os.makedirs(limits.BRIEFS, exist_ok=True)
    for letter, pk in packages.items():
        rc, out = P.git(land, "worktree", "add", pk["tree"], "-b", pk["branch"], branch)
        if rc:
            refuse(f"дерево {pk['tree']} не создано: {out[-300:]} (уже созданное оставлено; повтори после исправления)")
        with open(pk["brief"], "w", encoding="utf-8") as f:
            f.write(brief_text(code, letter, pk, design, d["contract"], d["then"], base))
    with P.locked() as s:
        s["features"][code] = {"stage": "code", "t": time.time(), "design": design, "base": base, "branch": branch,
                               "contract": d["contract"], "packages": packages, "note": ""}
        P.queue_board(s, code, "В работе", f"дизайн {design}; пакетов {len(packages)}; ветка {branch}")
    event("plan", code, "", f"packages={len(packages)}")
    print(f"[studio] {code}: контракт {base[:9]} ({len(d['contract'])} файл(ов)), пакетов {len(packages)}. Запусти ВСЕ одним "
          "сообщением:")
    for letter, pk in packages.items():
        print(launch_line(code, letter, pk))


def cmd_launch(args):
    s = P.load()
    feat = s["features"].get(args[0] if args else "")
    if not feat:
        refuse("studio.py launch CODE — такой фичи в полёте нет")
    todo = [(l, pk) for l, pk in sorted(feat["packages"].items()) if pk["state"] == "planned"]
    for letter, pk in todo:
        print(launch_line(args[0], letter, pk))
    if not todo:
        print(f"[studio] {args[0]}: незапущенных пакетов нет")


# ---- accept ---------------------------------------------------------------------------------------------------------
def spend_row(code, kind, outcome, aid):
    """One ledger row from the agent's transcript (agent_spend.analyse); the text of the failure when there is none."""
    try:
        import agent_spend
    except ImportError as e:
        return None, f"agent_spend.py не загружен: {e}"
    hits = glob.glob(os.path.expanduser(f"~/.claude/projects/*/*/subagents/agent-{aid}.jsonl"))
    if not hits:
        return None, f"нет транскрипта агента {aid}"
    n, rows, totals, _ = agent_spend.analyse(max(hits, key=os.path.getsize))
    cost = lambda r: 5 * r["out"] + 1.25 * (r["added"] + r["rebuilt"]) + 0.1 * r["carried"]
    total = sum(cost(r) for r in rows.values()) or 1
    share = lambda *names: round(100 * sum(cost(rows[k]) for k in rows if any(k.startswith(x) for x in names)) / total)
    denies = 0
    try:
        with open(limits.GUARD_LOG, encoding="utf-8", errors="replace") as f:
            denies = sum(1 for line in f if aid in line and '"deny"' in line)
    except OSError:
        pass
    row = {"code": code, "kind": kind, "outcome": outcome, "requests": n, "cost_units": int(total),
           "rebuilt_after_expiry": totals["rebuilt"], "search_pct": share("Bash: search", "Grep", "Glob"),
           "read_pct": share("Bash: read", "Read"), "build_pct": share("Bash: build"),
           "explore_calls": rows.get("Agent", {}).get("calls", 0), "guard_calls_logged": 0, "guard_denies": denies,
           "agent_id": aid}
    new = not os.path.exists(limits.LEDGER)
    os.makedirs(limits.STUDIO_HOME, exist_ok=True)
    with open(limits.LEDGER, "a", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=list(row))
        if new:
            w.writeheader()
        w.writerow(row)
    return row, ""


def frame_lines(frames):
    out = []
    for f in frames:
        if not os.path.isfile(f):
            out.append(f"frame MISSING: {f}")
            continue
        size, age = os.path.getsize(f), (time.time() - os.path.getmtime(f)) / 60
        dims = "?"
        try:
            from PIL import Image
            dims = "%dx%d" % Image.open(f).size
        except Exception:  # no PIL or not an image: the size line still tells
            pass
        out.append(f"frame {os.path.basename(f)}: {dims} {size // 1024} KB, {age:.0f} min old" +
                   (" LIKELY BLANK" if size < limits.BLANK_FRAME_BYTES else ""))
    return out


def scenario_lines(code):
    scn = f"{limits.SCRATCH}/{code}/frames/scn-latest"
    if not os.path.isdir(scn):
        return [f"scenario: НЕТ прогона play_scenario.py для {code} — видимое не доказано"]
    out = []
    try:
        out.append("scenario: " + open(os.path.join(scn, "result.txt"), encoding="utf-8", errors="replace").read().strip()[:200])
        for line in open(os.path.join(scn, "index.txt"), encoding="utf-8", errors="replace").read().splitlines()[:8]:
            name, _, what = line.partition("\t")
            if name:
                out.append(f"  look: {scn}/{name} — {what}")
    except OSError as e:
        out.append(f"scenario: {scn} не читается ({e})")
    return out


def cmd_accept(args):
    if not args:
        refuse("studio.py accept CODE[-PKG] [done|wip|stuck|refused] [FRAME...]")
    code = args[0]
    outcome = args[1] if len(args) > 1 and not os.path.exists(args[1]) else "done"
    frames = [a for a in args[1:] if a != outcome]
    out = []
    with P.locked() as s:
        aid, run = P.run_of(s, code)
        feat_code, letter = P.split_code(s, code)
        pk = s["features"][feat_code]["packages"][letter] if letter else None
        tree = pk["tree"] if pk else f"{limits.WORKTREE_ROOT}/{code}"
        kind = (run or {}).get("type") or "fix"
        if os.path.isfile(os.path.join(tree, "REMAINDER.md")):
            outcome = "wip"  # SPEND-2X: the run was closed by the call/context cap; its package is not done
        if aid:
            row, why = spend_row(code, kind, outcome, aid)
            out.append(f"{code} {outcome}: {row['cost_units'] / 1e6:.2f}M units, {row['requests']} req, read {row['read_pct']}% "
                       f"search {row['search_pct']}%, guard denies {row['guard_denies']}" if row else f"ledger: {why}")
            run["accepted"] = time.time()
            run["t1"] = run.get("t1") or time.time()
            run["outcome"] = outcome
        else:
            out.append(f"{code}: захода с таким кодом в реестре нет (запуск шёл мимо хука?) — принимаю только дерево")
        head = ""
        if os.path.isdir(tree):
            _, br = P.git(tree, "branch", "--show-current")
            _, head = P.git(tree, "rev-parse", "HEAD")
            _, when = P.git(tree, "log", "-1", "--format=%h %cr")
            rc, unp = P.git(tree, "rev-list", "--count", f"origin/{br}..HEAD")
            _, dirty = P.git(tree, "status", "--porcelain", "--untracked-files=no")
            out.append(f"tree {os.path.basename(tree)}: {br} @ {when}, unpushed {'not-on-origin' if rc else unp}, "
                       f"dirty {len([l for l in dirty.splitlines() if l.strip()])}")
        rem = tree + "/REMAINDER.md"
        if os.path.isfile(rem):  # SPEND-2X: the run hit limits.FEATURE_CALLS / CONTEXT_MAX; the rest goes to a fresh, small context
            outcome = "wip"
            out.append(f"REMAINDER.md в {tree}: свежий агент с тем же брифом, остаток — первым чтением. Запуск:")
            out.append(launch_line(feat_code, letter, pk).replace("Read it first;", f"Read it, then {rem} (the rest of the work; delete it in your first commit);")
                       if pk else f'Agent(subagent_type: "fixer", run_in_background: true, prompt: "You are agent {code}. Your brief: '
                                   f'{limits.BRIEFS}/{code}-BRIEF.md. Then {rem}: the rest of the work; delete it in your first commit.")')
        stalls = s.setdefault("stalls", {})
        st = stalls.setdefault(code, {"head": "", "n": 0})
        st["n"] = st["n"] + 1 if outcome == "stuck" or (head and head == st["head"]) else 0
        st["head"] = head or st["head"]
        if st["n"] >= limits.STALL_PASSES:
            out.append(f"STALLED {code}: {st['n']} сдачи без нового коммита — следующий запуск только с «## Пересмотр» в брифе")
        status, clean = ("На проверке" if outcome == "done" else "В работе"), True
        if pk:
            bad, stray = P.ownership(s, feat_code, letter)
            pk["strays"], pk["head"] = stray, head
            if bad:
                clean = False
                out.append("ВЛАДЕНИЕ НАРУШЕНО: " + ", ".join(f"{f} ({'контракт' if o == 'contract' else 'пакет ' + o})" for f, o in bad[:6]) +
                           " — вернуть агенту, слияние откажет")
            if stray:
                out.append(f"вне плана владения ({len(stray)}): " + ", ".join(stray[:6]) + " — впиши владельца в дизайн или верни")
            pk["state"] = "accepted" if outcome == "done" and clean else "planned"
            feat = s["features"][feat_code]
            if all(p["state"] == "accepted" for p in feat["packages"].values()) and feat["stage"] == "code":
                feat["stage"] = "review"
                out.append(f"{feat_code}: все пакеты приняты (ревью прошло в цепочке пакета) → один Agent(lander) на пачку: merge, сборка, приёмка, посадка")
            left = [l for l, p in feat["packages"].items() if p["state"] != "accepted"]
            P.queue_board(s, feat_code, CARD_OF_STAGE[feat["stage"]],
                          f"пакет {letter} {outcome}" + (f"; осталось пакетов: {' '.join(left)}" if left else "; код весь, ждёт ревью/сборки"))
        else:
            P.queue_board(s, code, status, f"заход {outcome}" + (f", {head[:9]}" if head else ""))
        n, age = P.board_debt(s)
    event("wip" if outcome == "wip" else "done", code, aid or "", "")
    out += frame_lines(frames) + scenario_lines(code)
    out.append(f"доска: в очереди {n} (пачка при limits.BOARD_BATCH = {limits.BOARD_BATCH}) — studio.py board")
    print("\n".join(out))


# ---- merge / build / accept / land ----------------------------------------------------------------------------------
def cmd_merge(args):
    if not args:
        refuse("studio.py merge CODE [CODE...]")
    land = limits.LAND_TREE
    if os.path.exists(os.path.join(land, ".git", "MERGE_HEAD")):
        refuse(f"в {land} идёт незаконченное слияние — закончи или отмени его")
    with P.locked() as s:
        for code in args:
            feat = s["features"].get(code)
            if not feat or feat["stage"] not in ("code", "review"):
                refuse(f"{code}: нет такой фичи на стадии code/review")
            waiting = [l for l, p in feat["packages"].items() if p["state"] != "accepted"]
            if waiting:
                refuse(f"{code}: пакеты не приняты: {' '.join(waiting)} — studio.py accept {code}-<ПАКЕТ>")
            for letter in sorted(feat["packages"]):
                bad, _ = P.ownership(s, code, letter)
                if bad:
                    refuse(f"{code}-{letter}: владение нарушено — {', '.join(f for f, _ in bad[:6])}")
            gaps = P.scenario_gaps(feat)
            if gaps:
                refuse(f"{code}: сценарий не покрывает приёмку дизайна — {'; '.join(gaps[:4])}. Владелец файла сценария "
                       "дописывает шаги одним заходом починки; без кадра утверждение «Тогда» нечем принять")
        merged = []
        for code in args:
            feat = s["features"][code]
            for letter, pk in sorted(feat["packages"].items()):
                rc, out = P.git(land, "merge", "--no-ff", "--no-edit", pk["branch"])
                if rc:
                    print(f"[studio] слияние {pk['branch']} остановилось: {out[-400:]}\nвлиты: {' '.join(merged) or 'ничего'}. "
                          "Разреши конфликт руками (пакеты не пересекаются по файлам — конфликт значит чужую правку) и повтори.")
                    sys.exit(2)
                merged.append(pk["branch"])
            feat["stage"] = "build"
            P.queue_board(s, code, "На проверке", "все пакеты влиты, идёт сборка пачки")
            event("merge", code)
    print(f"[studio] влито {len(merged)} веток в {land}. ОДНА сборка пачки (в фоне): "
          f"bash .claude/tools/build_land.sh {land} <лог> ; затем studio.py built ok|fail")


def cmd_built(args):
    if not args or args[0] not in ("ok", "fail"):
        refuse("studio.py built ok|fail [note]")
    with P.locked() as s:
        hit = [c for c, f in s["features"].items() if f["stage"] == "build"]
        for code in hit:
            feat = s["features"][code]
            feat["stage"] = "accept" if args[0] == "ok" else "build"
            feat["note"] = "" if args[0] == "ok" else "сборка красная: " + " ".join(args[1:])[:160]
            event("built-" + args[0], code)
    print(f"[studio] сборка {args[0]}: {' '.join(hit) or 'нет фич на стадии build'}" +
          (" → acceptance по сценариям, затем studio.py passed <КОД>" if args[0] == "ok" else
           " → починка в дереве посадки одним заходом (код <КОД>-FIX), та же сборка ещё раз"))


def cmd_passed(args):
    with P.locked() as s:
        feat = s["features"].get(args[0] if args else "")
        if not feat or feat["stage"] != "accept":
            refuse("studio.py passed CODE — фича должна быть на стадии accept")
        feat["stage"] = "land"
        event("passed", args[0])
    print(f"[studio] {args[0]}: приёмка пройдена → scripts/Dev/land_dev.sh, затем studio.py landed {args[0]}")


def cmd_landed(args):
    land = limits.LAND_TREE
    with P.locked() as s:
        feat = s["features"].get(args[0] if args else "")
        if not feat:
            refuse("studio.py landed CODE")
        P.git(land, "fetch", "origin", "dev", "--quiet")
        out_of_dev = [pk["branch"] for pk in feat["packages"].values()
                      if P.git(land, "merge-base", "--is-ancestor", pk["branch"], limits.TRUNK)[0] != 0]
        if out_of_dev:
            refuse(f"{args[0]}: не в {limits.TRUNK}: {' '.join(out_of_dev)} — посадка не закончена")
        _, sha = P.git(land, "rev-parse", "--short=9", limits.TRUNK)
        left = []
        for pk in feat["packages"].values():
            if os.path.isdir(pk["tree"]) and P.git(land, "worktree", "remove", pk["tree"])[0] != 0:
                left.append(pk["tree"])
            P.git(land, "branch", "-d", pk["branch"])
        P.git(land, "branch", "-d", feat["branch"])
        arch = P.archive_design(args[0], feat)
        feat["stage"], feat["landed"] = "done", sha
        P.queue_board(s, args[0], "Сделано", f"Коммит {sha}; В dev = __YES__" +
                      (f"; дизайн на Вики из {arch}" if arch else "; ДИЗАЙН НЕ НАЙДЕН — на Вики класть нечего"))
        event("landed", args[0], "", sha)
    print(f"[studio] {args[0]}: в dev {sha}; деревья и ветки убраны" +
          (f"; НЕ убраны (грязные, без --force): {' '.join(left)}" if left else "") + ". Карточка — в очереди доски.")


def cmd_drop(args):
    if len(args) < 2:
        refuse('studio.py drop CODE "причина"')
    with P.locked() as s:
        feat = s["features"].get(args[0])
        if not feat:
            refuse(f"{args[0]}: такой фичи нет")
        feat["stage"], feat["note"] = "dropped", " ".join(args[1:])[:200]
        P.queue_board(s, args[0], "Заблокировано", "снята с полёта: " + feat["note"])
        event("dropped", args[0])
    print(f"[studio] {args[0]} снята с полёта; деревья {limits.WORKTREE_ROOT}/{args[0]}-* остались до решения по ветке")


# ---- board / night / wait / report / migrate ------------------------------------------------------------------------
def cmd_board(_args):
    """The queue goes straight to Notion through notion_board.py (REST, zero model tokens); a sent item leaves the queue."""
    import notion_board
    with P.locked() as s:
        q = list(s["board_queue"])
    if not q:
        print("[studio] очередь доски пуста")
        return
    path = os.path.join(limits.STUDIO_HOME, "board_batch.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump([{"code": x["code"], "status": x["status"], "line": x["line"]} for x in q], f, ensure_ascii=False)
    try:
        notion_board.cmd_apply(path)
        notion_board.cmd_sync()
    except SystemExit:
        refuse("доска не обновлена (строка notion_board выше); очередь оставлена")
    sent = {(x["code"], x["t"]) for x in q}
    with P.locked() as s:
        s["board_queue"] = [x for x in s["board_queue"] if (x["code"], x["t"]) not in sent]
    print(f"[studio] доска: обновлено {len(q)}")


def cmd_night(args):
    with P.locked() as s:
        if args[:1] == ["on"]:
            hh, mm = (args[1].split(":") + ["0"])[:2] if len(args) > 1 else ("8", "0")
            t = time.localtime()
            until = time.mktime((t.tm_year, t.tm_mon, t.tm_mday, int(hh), int(mm), 0, 0, 0, -1))
            s["night_until"] = until if until > time.time() else until + 86400
            print("[studio] ночной режим до " + time.strftime("%d.%m %H:%M", time.localtime(s["night_until"])))
        else:
            s["night_until"] = 0
            print("[studio] ночной режим выключен")


def cmd_wait(args):
    if not args:
        refuse("studio.py wait FILE [--marker TEXT]")
    path, marker = args[0], (args[args.index("--marker") + 1] if "--marker" in args[:-1] else "")
    end, size, still = time.time() + limits.WAIT_SLICE, -1, 0
    state = "ещё идёт"
    while time.time() < end:
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            text = ""
        if marker and marker in text:
            state = "маркер найден"
            break
        still = still + 1 if len(text) == size else 0
        size = len(text)
        if not marker and still >= 4:
            state = "вывод не растёт 20 с"
            break
        time.sleep(5)
    tail = "\n".join(text.splitlines()[-5:]) if text else "(пусто)"
    print(f"[studio] {path}: {state}\n{tail[-800:]}")


def cmd_report(_args):
    import collections
    import statistics
    rows = lambda p: list(csv.DictReader(open(p, encoding="utf-8"))) if os.path.exists(p) else []
    evs, led = rows(limits.EVENTS), {r["agent_id"]: r for r in rows(limits.LEDGER)}
    launch = {e["agent_id"]: e for e in evs if e["event"] == "launch" and e["agent_id"]}
    end = {e["agent_id"]: e for e in evs if e["event"] in ("done", "wip") and e["agent_id"]}
    by = collections.defaultdict(list)
    for aid, e in launch.items():
        by[e["tags"] or "untagged"].append(aid)
    for tag, aids in sorted(by.items()):
        got = [led[a] for a in aids if a in led]
        if not got:
            continue
        mins = [(int(end[a]["ts"]) - int(launch[a]["ts"])) / 60 for a in aids if a in end]
        print(f"{tag:14} n={len(got):3} {sum(int(r['cost_units']) for r in got) / len(got) / 1e6:.2f}M/agent "
              f"{sum(int(r['requests']) for r in got) / len(got):4.0f} calls wip={sum(r['outcome'] == 'wip' for r in got)}/{len(got)}" +
              (f" launch->end median {statistics.median(mins):.0f} min" if mins else ""))
    plan = {e["code"]: int(e["ts"]) for e in evs if e["event"] == "plan"}
    lead = [(int(e["ts"]) - plan[e["code"]]) / 3600 for e in evs if e["event"] == "landed" and e["code"] in plan]
    if lead:
        print(f"plan -> dev: median {statistics.median(lead):.1f} h over {len(lead)} feature(s), max {max(lead):.1f} h")


def cmd_migrate(args):
    dry = "--dry-run" in args
    home = os.path.expanduser("~/.claude")
    moves = [(f"{home}/process-events.csv", limits.EVENTS), (f"{home}/agent-spend-ledger.csv", limits.LEDGER),
             (f"{home}/agent-guard/log.jsonl", limits.GUARD_LOG)]
    moves += [(p, f"{limits.STUDIO_HOME}/{os.path.basename(p)}") for p in glob.glob(f"{home}/tools/auto_compact.*")]
    moves += [(f"{home}/lead-spend.csv", f"{limits.STUDIO_HOME}/lead-spend.csv"),
              (f"{home}/lead-watch", f"{limits.STUDIO_HOME}/lead-watch")]
    drops = ([f"{home}/tools/accept.sh", f"{home}/tools/ledger.py", f"{home}/tools/process_event.py", f"{home}/night_mode"] +
             glob.glob(f"{home}/board_guard_state.json.*.tmp"))
    done = []
    os.makedirs(limits.STUDIO_HOME, exist_ok=True)
    old_board = f"{home}/board_guard_state.json"
    if os.path.exists(old_board) and not os.path.exists(limits.BOARD_STATE):
        done.append(f"board mirror {old_board} -> {limits.BOARD_STATE} (pages, writes, board, board_t)")
        if not dry:
            old = json.load(open(old_board, encoding="utf-8"))
            with open(limits.BOARD_STATE, "w", encoding="utf-8") as f:
                json.dump({k: old.get(k, {} if k != "board_t" else 0) for k in ("pages", "writes", "board", "board_t")}, f)
        drops.append(old_board)
    for src, dst in moves:
        if os.path.exists(src) and not os.path.exists(dst):
            done.append(f"move {src} -> {dst}")
            if not dry:
                shutil.move(src, dst)
    stall = f"{home}/stall"
    if os.path.isdir(stall):
        done.append(f"stall counters {stall} -> pipeline.json")
        if not dry:
            with P.locked() as s:
                for name in os.listdir(stall):
                    if not name.endswith(".head"):
                        try:
                            n = int(open(os.path.join(stall, name)).read().strip() or 0)
                        except (OSError, ValueError):
                            n = 0
                        if n:
                            s.setdefault("stalls", {})[name] = {"head": "", "n": n}
            shutil.rmtree(stall, ignore_errors=True)
    for path in drops + [f"{home}/agent-guard"]:
        if os.path.exists(path):
            done.append(f"delete {path}")
            if not dry:
                shutil.rmtree(path, ignore_errors=True) if os.path.isdir(path) else os.remove(path)
    print("[studio] migrate" + (" (dry-run)" if dry else "") + ":\n  " + ("\n  ".join(done) or "нечего переносить"))


def cmd_digest(_args):
    s = P.load()
    lines = [P.digest(s)]
    try:
        board = json.load(open(limits.BOARD_STATE, encoding="utf-8")).get("board", {})
        by = {}
        for row in board.values():
            if row.get("status") in ("В работе", "Готово к старту", "На проверке", "Заблокировано"):
                by[row["status"]] = by.get(row["status"], 0) + 1
        if by:
            lines.append("  доска (зеркало): " + "; ".join(f"{k} {v}" for k, v in sorted(by.items())) +
                         " — разбор хвостов одной пачкой через board, запуск они не держат")
    except (OSError, ValueError):
        lines.append("  зеркала доски нет: первый запуск board в сессии — TAILS-запрос (.claude/agents/board.md)")
    ready = P.next_work(s)
    if ready:
        lines.append("  к запуску: " + " ".join(ready[:8]))
    print("\n".join(lines))


COMMANDS = {"digest": cmd_digest, "plan": cmd_plan, "launch": cmd_launch, "accept": cmd_accept, "merge": cmd_merge,
            "built": cmd_built, "passed": cmd_passed, "landed": cmd_landed, "drop": cmd_drop, "board": cmd_board,
            "night": cmd_night, "wait": cmd_wait, "report": cmd_report, "migrate": cmd_migrate}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        sys.exit(2)
    COMMANDS[sys.argv[1]](sys.argv[2:])
