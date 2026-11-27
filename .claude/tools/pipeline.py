#!/usr/bin/env python3
"""pipeline — the feature pipeline's state and the rules read from a design file. One owner for each:

  * the design format («## Порядок» = «### Контракт» + 1..MAX_PACKAGES «### Пакет A..» with a «Файлы:» and a
    «Закрывает:» line; every numbered difference of «## Отличия» is closed by exactly one package; «## Приёмка»
    names the .scn file and one package owns it):
    parse_design / design_errors — used by the architect's hand-in check, `studio.py plan` and the engineer launch;
  * file ownership: owner_of / overlap / violations — used at launch, on every Edit/Write and at accept/merge;
  * who runs now: the run registry (register_launch / mark_stopped / live_runs), written only by agent_guard;
  * what is in flight: features and their stage, written only by studio.py.

State: limits.PIPELINE (JSON), changed only inside `with locked() as s:`.
  {"v": 1, "night_until": epoch,
   "features": {CODE: {"stage": design|code|review|build|accept|land|done|dropped, "t": epoch, "design": path,
                       "base": sha, "contract": [path], "branch": "task/CODE", "note": str,
                       "packages": {"A": {"name": str, "files": [pattern], "tree": dir, "branch": "task/CODE-A",
                                          "brief": path, "state": planned|running|handed|accepted, "head": sha,
                                          "stall": n, "strays": [path]}}}},
   "runs": {agent_id: {"code": str, "type": str, "t0": epoch, "t1": epoch|0, "accepted": epoch|0, "outcome": str}},
   "stalls": {code: {"head": sha, "n": hand-ins without a new commit}},
   "board_queue": [{"code": str, "status": str, "line": str, "t": epoch, "sent": epoch|0}]}
Events: limits.EVENTS (CSV: ts, event, code, agent_id, tags), appended only by event().
"""
import contextlib
import csv
import fnmatch
import json
import os
import re
import shutil
import subprocess
import time

import limits

DESIGN_KEYS = ("Эталон", "Отличия", "Архитектура", "Порядок", "Приёмка")
STAGES = ("design", "code", "review", "build", "accept", "land", "done", "dropped")
NEXT = {"design": "studio.py plan {c} <design.md>",
        "code": "запусти пакеты (studio.py launch {c}); на сдачу — studio.py accept {c}-<ПАКЕТ>",
        "review": "reviewer на каждый пакет параллельно; затем studio.py merge {c}",
        "build": "bash .claude/tools/build_land.sh <дерево> <лог>; затем studio.py built ok|fail",
        "accept": "агент acceptance по Scenarios; затем studio.py passed {c}",
        "land": "scripts/Dev/land_dev.sh; затем studio.py landed {c}"}
NOT_COUNTED = ("explore", "board")
WRITERS = ("engineer", "fixer", "general-purpose")  # their hand-in is accepted by `studio.py accept`

_PKG = re.compile(r"^### Пакет ([A-Z])\b[ \t]*(?:[—:–-][ \t]*)?([^\n]*)\n(.*?)(?=^### |^## |\Z)", re.S | re.M)
_CONTRACT = re.compile(r"^### Контракт[^\n]*\n(.*?)(?=^### |^## |\Z)", re.S | re.M)
_FILES = re.compile(r"^\s*Файлы:\s*(.+)$", re.M)
_CLOSES = re.compile(r"^\s*Закрывает:\s*(.+)$", re.M)
_ID_RANGE = re.compile(r"([A-ZА-ЯЁ]{1,3})(\d{1,3})(?:\s*[-–—]\s*(?:[A-ZА-ЯЁ]{1,3})?(\d{1,3}))?")
_DIFF = re.compile(r"^\s*(?:[-*]\s+|\|\s*|#{3,4}\s+)?\*{0,2}([A-ZА-ЯЁ]{1,3}\d{1,3})\b[^\n]*$", re.M)
_SCN = re.compile(r"Scenarios/[\w./-]+\.scn")
_FRAME = re.compile(r"\[([\w.-]+)\]")
# A network scenario names the role first («Тогда A net_bytes …»), so both leading words may be the frame.
_SCN_STEP = re.compile(r"^\s*(?:Тогда(?:-вид)?|Then(?:-view)?)\s+(\S+)(?:\s+(\S+))?", re.M)
UNPROVEN = "НЕ ДОКАЗАНО"
TOOLING = (".claude/", "scripts/", "CLAUDE.md")  # a design over these only has no editor scenario
_PATH = re.compile(r"[A-Za-z0-9_.][A-Za-z0-9_./*\[\]?+-]*")
_THEN = re.compile(r"^\s*(?:[-*]\s*)?(?:Тогда|Then)[^\n]*\[[\w.-]+\][^\n]*$", re.M)
# A plan that is a queue, not a parallel split (owner 11-26: «не растягивать задачу на 1000 лет»).
SERIAL = [re.compile(p, re.I) for p in (
    r"\b(?:[2-9]|\d{2,})\s*(?:дн[яей]\w*|days)\b", r"\b(?:день|day)\s*[2-9]\b", r"следующ\w+\s+(?:день|дня|заход\w*)",
    r"\b(?:после)?завтра\b|\btomorrow\b", r"\bнедел\w*|\bweeks?\b", r"заход\w*\s+\d+\s*(?:из|/)\s*\d+",
    r"(?:после|after)\s+(?:пакета|package)\s+[A-Z]\b", r"пакет\s+[A-Z]\s+(?:жд[её]т|после)\b",
    r"заход\w*\s+(?:идут\s+)?(?:строго\s+)?последовательн|последовательн\w+\s+заход|\bserial runs?\b")]


def section(text, name):
    m = re.search(rf"^## {name}[^\n]*\n(.*?)(?=^## |\Z)", text, re.S | re.M)
    return m.group(1) if m else None


def norm_pat(p):
    p = p.strip().strip("`'\" ").replace("\\", "/")
    p = p[2:] if p.startswith("./") else p
    return p[:-2] if p.endswith("/**") else p


def _wild(p):
    return p.endswith("/") or any(c in p for c in "*?[")


def matches(path, pat):
    path, pat = path.replace("\\", "/").lower(), pat.lower()
    return path.startswith(pat) if pat.endswith("/") else fnmatch.fnmatchcase(path, pat)


def overlap(p, q):
    """True when some file can match both patterns (exact file, `Dir/` subtree, or a glob)."""
    if not _wild(p) and not _wild(q):
        return p.lower() == q.lower()
    if not _wild(p):
        return matches(p, q)
    if not _wild(q):
        return matches(q, p)
    lp, lq = re.split(r"[*?\[]", p, 1)[0].lower(), re.split(r"[*?\[]", q, 1)[0].lower()
    if not (lp.startswith(lq) or lq.startswith(lp)):
        return False
    sp = "" if p.endswith("/") else re.split(r"[*?\]]", p)[-1].lower()
    sq = "" if q.endswith("/") else re.split(r"[*?\]]", q)[-1].lower()
    return sp.endswith(sq) or sq.endswith(sp)


def contract_dir(design_path):
    """Where the architect puts the contract headers in full: <design without .md>-contract/<repo-relative path>."""
    return re.sub(r"\.md$", "", design_path.replace("\\", "/")) + "-contract"


def parse_design(path):
    """{"text", "contract": [path], "packages": {letter: {"name", "files": [pattern], "body"}}, "then": [line]}"""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    order = section(text, "Порядок") or ""
    contract = []
    m = _CONTRACT.search(order)
    if m:
        for line in m.group(1).splitlines():
            line = line.strip()
            if line.startswith(("-", "*")):
                tok = _PATH.search(line.lstrip("-* `"))
                if tok and ("/" in tok.group(0)):
                    contract.append(norm_pat(tok.group(0)))
    packages, twice = {}, []
    for letter, name, body in _PKG.findall(order):
        if letter in packages:
            twice.append(letter)
        fm = _FILES.search(body)
        files = [norm_pat(x) for x in fm.group(1).split(",") if norm_pat(x)] if fm else []
        cm = _CLOSES.search(body)
        packages[letter] = {"name": name.strip(), "files": files, "body": body.strip(),
                            "closes": expand_ids(cm.group(1)) if cm else []}
    accept = section(text, "Приёмка") or ""
    diffs = {}
    for m in _DIFF.finditer(section(text, "Отличия") or ""):
        diffs.setdefault(m.group(1), UNPROVEN in m.group(0))
    return {"text": text, "order": order, "contract": contract, "packages": packages, "twice": twice,
            "then": [l.strip() for l in _THEN.findall(accept)], "diffs": diffs,
            "scenarios": sorted(set(_SCN.findall(accept)))}


def expand_ids(text):
    """«D1, D3-D5, W7–9» -> [D1, D3, D4, D5, W7, W8, W9]."""
    out = []
    for prefix, a, b in _ID_RANGE.findall(text):
        for n in range(int(a), int(b or a) + 1):
            if prefix + str(n) not in out:
                out.append(prefix + str(n))
    return out


def trace_errors(d):
    """Spec Kit `analyze` / OpenSpec `validate`, in our format: no difference without a package, no package without one."""
    errs, pk, diffs = [], d["packages"], d["diffs"]
    if not diffs:
        return ["в «## Отличия» нет нумерованных отличий: каждая строка-отличие начинается с номера (D1, D2 …)"]
    owners = {}
    for letter in sorted(pk):
        if not pk[letter]["closes"]:
            errs.append(f"пакет {letter} без строки «Закрывает: D1, D2 …» — неизвестно, какие отличия он закрывает")
        for i in pk[letter]["closes"]:
            owners.setdefault(i, []).append(letter)
    ghost = [i for i in owners if i not in diffs]
    if ghost:
        errs.append("«Закрывает» называет отличия, которых нет в «## Отличия»: " + ", ".join(ghost[:8]))
    twice = [f"{i} ({'+'.join(l)})" for i, l in owners.items() if len(l) > 1]
    if twice:
        errs.append("отличие закрывают два пакета: " + ", ".join(twice[:8]) + " — у отличия один владелец")
    lost = [i for i, unproven in diffs.items() if i not in owners and not unproven]
    if lost:
        errs.append("отличия без пакета: " + ", ".join(lost[:12]) + " — отличие, оставленное без владельца, не будет "
                    f"сделано (допустимо только с «{UNPROVEN}» и названным опытом)")
    return errs


def scenario_errors(d):
    """OpenSpec ties a scenario to its requirement; ours is an executable file, so the tie is a path with one owner."""
    pk = d["packages"]
    if all(f.startswith(TOOLING) for p in pk.values() for f in p["files"]):
        return []
    if not d["scenarios"]:
        return ["«## Приёмка» не называет файл сценария Scenarios/<Подсистема>/<КОД>.scn"]
    errs = []
    for scn in d["scenarios"]:
        own = [l for l in sorted(pk) if any(matches(scn, f) for f in pk[l]["files"])]
        if len(own) != 1:
            errs.append(f"сценарий {scn}: владельцев {len(own)} — файл сценария входит в «Файлы:» ровно одного пакета")
    return errs


def scenario_gaps(feat):
    """Frames the design promises («Тогда [кадр]») that the written scenario does not shoot. [] = covered."""
    try:
        d = parse_design(feat.get("design") or "")
    except OSError:
        return [f"дизайн {feat.get('design')} не читается"]
    want = [m.group(1) for line in d["then"] for m in [_FRAME.search(line)] if m]
    if not d["scenarios"] or not want:
        return []
    have, gaps = set(), []
    for scn in d["scenarios"]:
        tree = next((p["tree"] for p in feat["packages"].values() if any(matches(scn, f) for f in p["files"])), "")
        try:
            with open(os.path.join(tree, scn), encoding="utf-8", errors="replace") as f:
                have.update(w for pair in _SCN_STEP.findall(f.read()) for w in pair if w)
        except OSError:
            gaps.append(f"{scn} не написан в дереве {tree or '?'}")
    lost = [w for w in want if w not in have]
    if lost and not gaps:
        gaps.append("нет шагов «Тогда <кадр>» для кадров: " + ", ".join(lost[:8]))
    return gaps


def archive_design(code, feat):
    """OpenSpec `archive`, ours: the landed design and its contract leave the scratch disk. Returns the folder or ''."""
    src = feat.get("design") or ""
    dst = f"{limits.ARCHIVE}/{time.strftime('%Y-%m-%d')}-{code}"
    try:
        os.makedirs(dst, exist_ok=True)
        shutil.copy2(src, dst + "/design.md")
        if os.path.isdir(contract_dir(src)):
            shutil.copytree(contract_dir(src), dst + "/contract", dirs_exist_ok=True)
    except OSError:
        return ""
    feat["design"] = dst + "/design.md"
    return dst


def design_errors(path):
    """Every reason the design may not be planned; [] when it is a parallel plan over disjoint files."""
    if not path or not os.path.isfile(path):
        return [f"файл дизайна {path} не найден"]
    d = parse_design(path)
    errs = []
    lost = [k for k in DESIGN_KEYS if section(d["text"], k) is None]
    if lost:
        return ["нет разделов: " + ", ".join("## " + k for k in lost)]
    pk = d["packages"]
    if not pk:
        errs.append("в «## Порядок» нет ни одного «### Пакет A — имя» со строкой «Файлы: …»")
    if len(pk) > limits.MAX_PACKAGES:
        errs.append(f"пакетов {len(pk)} > limits.MAX_PACKAGES ({limits.MAX_PACKAGES}): раздели фичу на две фичи с общим "
                    "контрактом или укрупни пакеты")
    if d["twice"]:
        errs.append("буква пакета повторяется: " + ", ".join(d["twice"]))
    for letter, p in pk.items():
        if not p["files"]:
            errs.append(f"пакет {letter} без строки «Файлы: …» — владение не задано")
    letters = sorted(pk)
    for i, a in enumerate(letters):
        for b in letters[i + 1:]:
            hit = [(x, y) for x in pk[a]["files"] for y in pk[b]["files"] if overlap(x, y)]
            if hit:
                errs.append(f"пересечение владения файлами: пакеты {a} и {b} оба берут {hit[0][0]} / {hit[0][1]} — "
                            "общий файл уходит в «### Контракт» или целиком одному пакету")
    serial = [m.group(0) for rx in SERIAL for m in [rx.search(d["order"])] if m]
    if serial:
        errs.append("последовательный план на дни («" + "», «".join(serial[:3]) + "»): пакеты идут ПАРАЛЛЕЛЬНО в один "
                    "день, друг от друга зависят только через «### Контракт» (владелец 11-26)")
    cdir = contract_dir(path)
    gone = [c for c in d["contract"] if not os.path.isfile(os.path.join(cdir, c))]
    if gone:
        errs.append(f"заголовки контракта не написаны целиком в {cdir}/: " + ", ".join(gone[:4]))
    if not d["then"]:
        errs.append("в «## Приёмка» нет строки «Тогда [кадр]: что видно»")
    return errs + trace_errors(d) + scenario_errors(d)


# ---- state ----------------------------------------------------------------------------------------------------------
def load():
    try:
        with open(limits.PIPELINE, encoding="utf-8") as f:
            s = json.load(f)
    except (OSError, ValueError):
        s = {}
    s.setdefault("v", 1)
    for k in ("features", "runs", "stalls"):
        s.setdefault(k, {})
    s.setdefault("board_queue", [])
    s.setdefault("night_until", 0)
    return s


def _save(s):
    now = time.time()
    for r in s["runs"].values():
        if not r.get("t1") and now - r.get("t0", 0) > limits.RUNNING_MAX_AGE_H * 3600:
            r["t1"], r["outcome"] = now, "died"  # its session ended without a stop: shown as handed in, not accepted
            if r.get("type") not in WRITERS:
                r["accepted"] = now
    live = {a: r for a, r in s["runs"].items() if not r.get("accepted")}
    done = sorted(((r.get("t0", 0), a) for a, r in s["runs"].items() if r.get("accepted")))[-limits.RUNS_KEEP:]
    s["runs"] = dict(live, **{a: s["runs"][a] for _, a in done})
    os.makedirs(limits.STUDIO_HOME, exist_ok=True)
    tmp = "%s.%d.tmp" % (limits.PIPELINE, os.getpid())
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(s, f, ensure_ascii=False, indent=1)
    os.replace(tmp, limits.PIPELINE)


@contextlib.contextmanager
def locked():
    """Read-modify-write under a lock file: parallel hooks of parallel agents write the same state."""
    os.makedirs(limits.STUDIO_HOME, exist_ok=True)
    lock = limits.PIPELINE + ".lock"
    held = False
    for _ in range(60):
        try:
            os.close(os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY))
            held = True
            break
        except FileExistsError:
            try:
                if time.time() - os.path.getmtime(lock) > 10:  # its owner died
                    os.remove(lock)
                    continue
            except OSError:
                pass
            time.sleep(0.05)
        except OSError:
            break
    s = load()
    try:
        yield s
        _save(s)
    finally:
        if held:
            try:
                os.remove(lock)
            except OSError:
                pass


# ---- runs -----------------------------------------------------------------------------------------------------------
def register_launch(s, agent_id, code, kind):
    s["runs"][agent_id] = {"code": code, "type": kind, "t0": time.time(), "t1": 0, "accepted": 0, "outcome": ""}
    feat, pkg = split_code(s, code)
    if pkg and kind == "engineer":
        s["features"][feat]["packages"][pkg]["state"] = "running"


def mark_stopped(s, agent_id, kind="", code=""):
    """The run handed in. A run the launch hook did not see (no agent id in the tool response) is recorded here."""
    run = s["runs"].get(agent_id)
    if not run and code:
        run = s["runs"][agent_id] = {"code": code, "type": kind, "t0": time.time(), "t1": 0, "accepted": 0, "outcome": ""}
    if not run or run.get("t1"):
        return None
    run["t1"] = time.time()
    if run["type"] not in WRITERS:
        run["accepted"] = run["t1"]  # a design, a review or an acceptance report is read, not accepted
    feat, pkg = split_code(s, run["code"])
    if pkg and run["type"] == "engineer" and s["features"][feat]["packages"][pkg].get("state") == "running":
        s["features"][feat]["packages"][pkg]["state"] = "handed"
    event("stop", run["code"], agent_id, run["type"])
    return run


def resume(s, agent_id):
    """SendMessage woke a stopped agent: it is live again."""
    run = s["runs"].get(agent_id)
    if run and run.get("t1"):
        run.update(t1=0, accepted=0, t0=time.time())
        feat, pkg = split_code(s, run["code"])
        if pkg and run["type"] == "engineer":
            s["features"][feat]["packages"][pkg]["state"] = "running"


def stall_of(s, code):
    return (s.get("stalls") or {}).get(code, {}).get("n", 0)


def event(name, code, agent_id="", tags=""):
    """One row of the pipeline's history; `studio.py report` reads it."""
    try:
        os.makedirs(limits.STUDIO_HOME, exist_ok=True)
        new = not os.path.exists(limits.EVENTS)
        with open(limits.EVENTS, "a", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            if new:
                w.writerow(["ts", "event", "code", "agent_id", "tags"])
            w.writerow([int(time.time()), name, code, agent_id, tags])
    except OSError:
        pass


def is_live(run):
    return not run.get("t1") and time.time() - run.get("t0", 0) < limits.RUNNING_MAX_AGE_H * 3600


def live_runs(s):
    return {a: r for a, r in s["runs"].items() if is_live(r) and r.get("type") not in NOT_COUNTED}


def run_of(s, code):
    """(agent_id, run) of the latest not-yet-accepted run launched as @p code (else the latest run), or (None, None).
    Two runs under one code (a fix, then its reviewer) are accepted one per call, oldest pending first never hidden."""
    runs = [(r.get("t0", 0), a) for a, r in s["runs"].items() if r.get("code") == code]
    pending = [h for h in runs if not s["runs"][h[1]].get("accepted")]
    stopped = [h for h in pending if s["runs"][h[1]].get("t1")]  # a reviewer still running must not take the engineer's accept
    hits = sorted(stopped or pending or runs)
    return (hits[-1][1], s["runs"][hits[-1][1]]) if hits else (None, None)


def unaccepted(s):
    """Codes of writer runs that stopped and were not accepted by `studio.py accept`."""
    return sorted({r["code"] for r in s["runs"].values()
                   if r.get("t1") and not r.get("accepted") and r.get("type") in WRITERS})


# ---- features and ownership -------------------------------------------------------------------------------------------
def split_code(s, code):
    """(feature, package letter) for CODE-A of a planned feature; (feature, None) for the feature; else (None, None)."""
    if code in s["features"]:
        return code, None
    m = re.match(r"^(.+)-([A-Z])(?:-(?:REV|FIX)\d*)?$", code or "")
    if m and m.group(1) in s["features"] and m.group(2) in s["features"][m.group(1)].get("packages", {}):
        return m.group(1), m.group(2)
    return None, None


def in_flight(s):
    return {c: f for c, f in s["features"].items() if f.get("stage") not in ("done", "dropped")}


def landing(s):
    return any(f.get("stage") in ("build", "accept", "land") for f in s["features"].values())


def package_of_path(s, path):
    """(feature, letter, repo-relative path) when @p path lies inside a package's tree."""
    p = (path or "").replace("\\", "/")
    if re.match(r"^/[a-z]/", p):
        p = p[1].upper() + ":" + p[2:]
    low = p.lower()
    for code, feat in in_flight(s).items():
        for letter, pk in feat.get("packages", {}).items():
            tree = pk.get("tree", "").lower().rstrip("/")
            if tree and low.startswith(tree + "/"):
                return code, letter, p[len(tree) + 1:]
    return None, None, None


def owner_of(feat, rel):
    """"contract", a package letter, or None for a file nobody owns."""
    if any(matches(rel, c) for c in feat.get("contract", [])):
        return "contract"
    for letter, pk in sorted(feat.get("packages", {}).items()):
        if any(matches(rel, x) for x in pk.get("files", [])):
            return letter
    return None


def edit_refusal(s, path):
    """Text when an Edit/Write inside a package tree touches a file that package does not own, else None."""
    code, letter, rel = package_of_path(s, path)
    if not code:
        return None
    owner = owner_of(s["features"][code], rel)
    if owner == "contract":
        return (f"{rel} — заголовок контракта {code}: пакет его не правит. Не хватает контракта — остановись и напиши в "
                "отчёте строку «CONTRACT-BREAK: файл — что нужно»; контракт меняет architect одним коммитом для всех пакетов.")
    if owner and owner != letter:
        return (f"{rel} принадлежит пакету {code}-{owner}, твой пакет {code}-{letter} (владение файлами, дизайн «Порядок»). "
                "Нужна правка там — строка «NEEDS: файл — что» в отчёте; свой код пиши в своих файлах.")
    return None


def overlap_refusal(s, feat_code, letter):
    """Text when the package's files overlap a package another LIVE run owns (any feature), else None."""
    mine = s["features"][feat_code]["packages"][letter]["files"]
    live_codes = {r["code"] for r in live_runs(s).values()}
    for code, feat in in_flight(s).items():
        for other, pk in feat.get("packages", {}).items():
            if (code, other) == (feat_code, letter) or f"{code}-{other}" not in live_codes:
                continue
            hit = [(x, y) for x in mine for y in pk.get("files", []) if overlap(x, y)]
            if hit:
                return (f"пересечение владения файлами: {feat_code}-{letter} берёт {hit[0][0]}, а им уже владеет живой заход "
                        f"{code}-{other} ({hit[0][1]}). Дождись его сдачи или перепланируй пакеты.")
    return None


def plan_refusal(s, code):
    """Text when a new feature may not enter the pipeline (WIP limit, an old unlanded feature), else None."""
    flying = {c: f for c, f in in_flight(s).items() if c != code}
    old = sorted(c for c, f in flying.items() if time.time() - f.get("t", 0) > limits.FEATURE_MAX_AGE_H * 3600)
    if old:
        return (f"не посажены дольше limits.FEATURE_MAX_AGE_H ({limits.FEATURE_MAX_AGE_H} ч): {', '.join(old)} — сначала "
                "доведи их до dev или сними честно: studio.py drop <КОД> \"причина\" (владелец 11-26).")
    if len(flying) >= limits.FEATURES_IN_FLIGHT:
        return (f"в полёте {len(flying)} фич (limits.FEATURES_IN_FLIGHT = {limits.FEATURES_IN_FLIGHT}): "
                f"{', '.join(sorted(flying))}. Сначала посади одну.")
    return None


# worktree/merge/checkout touch the whole tree: 60 s timed out on the Windows HDD
SLOW_GIT = {"worktree", "merge", "checkout"}


def git(tree, *args):
    try:
        r = subprocess.run(["git", "-C", tree] + list(args), capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=600 if args and args[0] in SLOW_GIT else 60)
        return r.returncode, r.stdout.strip()
    except (OSError, subprocess.SubprocessError) as e:
        return 1, str(e)


def changed_files(tree, base):
    """Repo-relative files the tree changed since @p base, committed or not."""
    _, committed = git(tree, "diff", "--name-only", f"{base}..HEAD")
    _, dirty = git(tree, "status", "--porcelain", "--untracked-files=all")
    files = set(committed.split("\n")) | {l[3:].split(" -> ")[-1].strip('"') for l in dirty.split("\n") if len(l) > 3}
    return sorted(f for f in files if f)


def ownership(s, feat_code, letter):
    """(violations [(file, owner)], strays [file]) of a package's tree against the design's ownership."""
    feat = s["features"][feat_code]
    pk = feat["packages"][letter]
    bad, stray = [], []
    for f in changed_files(pk["tree"], feat.get("base") or limits.TRUNK):
        owner = owner_of(feat, f)
        if owner is None:
            stray.append(f)
        elif owner != letter:
            bad.append((f, owner))
    return bad, stray


# ---- board queue and digest -------------------------------------------------------------------------------------------
def queue_board(s, code, status, line):
    s["board_queue"] = [q for q in s["board_queue"] if q["code"] != code or q.get("sent")]
    s["board_queue"].append({"code": code, "status": status, "line": line[:300], "t": time.time(), "sent": 0})


def board_debt(s):
    """(count, age of the oldest in hours) of card updates not yet written to the board."""
    q = s["board_queue"]
    return len(q), ((time.time() - min(x["t"] for x in q)) / 3600 if q else 0)


def floor():
    try:
        with open(limits.FLOOR_FILE) as f:
            return int(f.read().strip() or 0)
    except OSError:
        return limits.DEFAULT_MIN_AGENTS
    except ValueError:
        return 0


def night(s):
    return s.get("night_until", 0) if s.get("night_until", 0) > time.time() else 0


def next_work(s):
    """Launchable codes: packages of features at stage `code` that are planned or handed in unfinished."""
    out = []
    for code, feat in sorted(in_flight(s).items(), key=lambda kv: kv[1].get("t", 0)):
        if feat.get("stage") == "code":
            out += [f"{code}-{l}" for l, pk in sorted(feat["packages"].items()) if pk.get("state") == "planned"]
    return out


def digest(s):
    """The lead's whole view of the pipeline in at most 15 lines; injected at every session start and compact."""
    lines = []
    flying = in_flight(s)
    live = live_runs(s)
    cap = limits.NIGHT_MAX_AGENTS if night(s) else limits.MAX_AGENTS
    n, age = board_debt(s)
    lines.append(f"[studio] фич в полёте {len(flying)}/{limits.FEATURES_IN_FLIGHT}; агентов {len(live)}/{cap} "
                 f"(пол {limits.NIGHT_MIN_AGENTS if night(s) else floor()}); доска: в очереди {n}"
                 + (f", старейшая {age:.1f} ч" if n else "") + ("; НОЧНОЙ РЕЖИМ" if night(s) else ""))
    for code, feat in sorted(flying.items(), key=lambda kv: kv[1].get("t", 0))[:8]:
        hours = (time.time() - feat.get("t", 0)) / 3600
        pk = " ".join(f"{l}:{p.get('state')}" for l, p in sorted(feat.get("packages", {}).items()))
        late = " ПРОСРОЧЕНА" if hours > limits.FEATURE_MAX_AGE_H else ""
        lines.append(f"  {code} [{feat.get('stage')}] {hours:.0f} ч{late} {pk} → " +
                     NEXT.get(feat.get("stage"), "").format(c=code) + (f" | {feat['note']}" if feat.get("note") else ""))
    wait = unaccepted(s)
    if wait:
        lines.append("  сдали, не приняты: " + " ".join(wait[:10]) + " → studio.py accept <КОД>")
    other = sorted({r["code"] for r in live.values()} - {f"{c}-{l}" for c, f in flying.items() for l in f.get("packages", {})})
    if other:
        lines.append("  идут вне фич: " + " ".join(other[:10]))
    return "\n".join(lines[:15])
