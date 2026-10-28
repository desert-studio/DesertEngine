#!/usr/bin/env python3
"""play_scenario.py <TREE> <SCENARIO.scn> [--hold] — the ONE way to check a feature live, on Mac and on Windows.

Owner 10-22: a card's spec is its scenario — Дано / Когда / Тогда, written BEFORE the code and committed at
<TREE>/Scenarios/<Subsystem>/<CODE>.scn. The same file is played on both machines (Mac agents, the Windows gate),
so a feature is proven the same way everywhere and the scenarios of every landed card are replayable later.

Line format (one step per line, `#` comments; the first comment is the title):
  Дано|Given  <DesertCtl args…>           setup step, run as `DesertCtl <args>` (shell quoting)
  Когда|When  <DesertCtl args…>           the action under test, same form
  Тогда|Then  <frame> <what it shows…>    capture the whole editor to <frame>.png
  Тогда-вид|Then-view <frame> <text…>     same, the 3D viewport only
  Ожидать|Expect …                        RESERVED until the owner turns tests back on (today the proof is frames)
  Проект|Project <x.deproj> [editor args…] BEFORE any step: the project the editor opens (UE's `UnrealEditor
                                          <x.uproject> <Map>`) and its launch args, e.g. `--scene Content/Scenes/A.desce`.
                                          The path is absolute, or relative to the tree, or to the tree's parent
                                          (a content project checked out beside the engine). Default: Projects/Desert.

A fresh editor is started through run_capped (one GPU slot per machine, it queues), the steps run in order — no
sleeps: every DesertCtl reply means the step finished — the first refused step stops the run and shoots fail.png,
then the editor is quit. Output: <scratch>/<CODE>/frames/scn-<time>/ with every frame, index.txt (frame → what it
must show), result.txt, editor.log and a copy of the scenario; scn-latest points at it. The digest is ≤ 3 lines.
--hold leaves the editor running after a passing run for inspection (quit it yourself: DesertCtl --socket <s> quit 0).
"""
import os
import shlex
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
WIN = os.name == "nt"
GIVEN = {"дано", "given", "когда", "when"}
THEN = {"тогда": "shot-window", "then": "shot-window", "тогда-вид": "shot-viewport", "then-view": "shot-viewport"}
EXPECT = {"ожидать", "expect"}
PROJECT = {"проект", "project"}


def project_line(tree, lines):
    """The scenario's `Проект` line -> (deproj path, extra editor args), or None for Projects/Desert."""
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        kw, _, rest = line.partition(" ")
        if kw.lower() not in PROJECT:
            return None  # a project named after the first step would be a different editor: refused below
        words = shlex.split(rest)
        if not words:
            sys.exit("SCENARIO: «Проект» needs a .deproj path")
        for base in (tree, os.path.dirname(tree)):  # an absolute path ignores the base; never cwd (main tree -> "New Scene", 10-28)
            cand = os.path.abspath(os.path.join(base, os.path.expanduser(words[0])))
            if os.path.isfile(cand):
                return cand, words[1:]
        sys.exit(f"SCENARIO: project {words[0]} found neither absolute, nor in {tree}, nor beside it")
    return None


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    tree, scn = os.path.abspath(args[0]), os.path.abspath(args[1])
    if os.path.commonpath([tree, scn]) != tree:
        # 10-28: a .scn from the agent's tree played against the main tree loaded nothing and still printed PASS.
        sys.exit(f"SCENARIO refused: {scn} is not inside the tree {tree} — pass the tree the scenario belongs to")
    hold = "--hold" in args[2:]
    code = os.path.splitext(os.path.basename(scn))[0]
    exe = ".exe" if WIN else ""
    bindir = os.path.join(tree, "build", "Bin", "Debug")
    editor, ctl = os.path.join(bindir, "Editor" + exe), os.path.join(bindir, "DesertCtl" + exe)
    lines = open(scn, encoding="utf-8").read().splitlines()
    named = project_line(tree, lines)
    proj, launch = named if named else (os.path.join(tree, "Projects", "Desert", "Desert.deproj"), [])
    for need in (scn, editor, ctl, proj):
        if not os.path.exists(need):
            sys.exit(f"SCENARIO {code}: missing {need}")

    scratch = os.environ.get("DESERT_SCRATCH") or os.path.join(os.path.dirname(REPO), "DesertEngine-scratch")
    framesdir = os.path.join(scratch, code, "frames")
    out = os.path.join(framesdir, "scn-" + time.strftime("%m%d-%H%M%S"))
    os.makedirs(out)
    latest = os.path.join(framesdir, "scn-latest")
    try:
        if os.path.islink(latest):
            os.remove(latest)
        os.symlink(out, latest)
    except OSError:  # Windows without symlink rights: a pointer file instead
        with open(latest + ".txt", "w", encoding="utf-8") as f:
            f.write(out + "\n")
    with open(os.path.join(out, "scenario.scn"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    sock = os.path.join(tempfile.gettempdir(), f"desert-scn-{code}.sock")
    if os.path.exists(sock):
        os.remove(sock)
    log = open(os.path.join(out, "editor.log"), "w", encoding="utf-8", errors="replace")
    env = dict(os.environ, DESERT_ROOT=tree, DESERT_SCENARIO="1")
    proc = subprocess.Popen([sys.executable, os.path.join(HERE, "run_capped.py"), editor, "--project", proj,
                             "--control-socket", sock, *launch], cwd=os.path.join(tree, "Editor"), env=env,
                            stdout=log, stderr=subprocess.STDOUT)

    def desertctl(*a):
        # --wait covers the machine's one GPU slot: run_capped queues the editor until it is free
        r = subprocess.run([ctl, "--socket", sock, "--wait", "2400", *a], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=2700)
        return r.returncode == 0, (r.stdout + r.stderr).strip().splitlines()[-1:] or [""]

    # run_capped may queue the editor for the GPU slot longer than DesertCtl's --wait: wait for the socket itself
    while not os.path.exists(sock) and proc.poll() is None:
        time.sleep(1)

    steps = frames = thens = 0
    fail = ""
    index = []
    for n, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        kw, _, rest = line.partition(" ")
        key = kw.lower()
        if key in THEN:
            thens += 1
        if fail:
            continue
        if key in PROJECT:
            if steps or frames:
                fail = f"line {n}: «{kw}» after the first step — the editor is already open on another project"
            continue
        if key in GIVEN:
            ok, msg = desertctl(*shlex.split(rest))
            if not ok:
                fail = f"line {n} ({kw} {rest}): {msg[0]}"
                continue
        elif key in THEN:
            name, _, text = rest.strip().partition(" ")
            ok, msg = desertctl(THEN[key], os.path.join(out, name + ".png"))
            if not ok:
                fail = f"line {n} (shot {name}): {msg[0]}"
                continue
            index.append(f"{name}.png\t{text.strip()}")
            frames += 1
        elif key in EXPECT:
            fail = f"line {n}: «Ожидать» is reserved until tests are back (owner 10-22: proof = frames)"
            continue
        else:
            fail = f"line {n}: unknown keyword «{kw}» (Дано/Когда/Тогда/Тогда-вид)"
            continue
        steps += 1

    with open(os.path.join(out, "index.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(index) + "\n")
    if fail:
        desertctl("shot-window", os.path.join(out, "fail.png"))
    if hold and not fail:
        print(f"SCENARIO {code}: held — editor PID {proc.pid}, socket {sock}; quit: {ctl} --socket {sock} quit 0")
    else:
        desertctl("quit", "0")
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
    log.close()
    with open(os.path.join(out, "editor.log"), encoding="utf-8", errors="replace") as f:
        errs = sum(1 for l in f if "error" in l.lower() or "assert" in l.lower())
    res = f"FAIL {fail}" if fail else f"PASS {steps} steps, {frames}/{thens} frames"
    res += f"; editor.log errors {errs}"
    with open(os.path.join(out, "result.txt"), "w", encoding="utf-8") as f:
        f.write(res + "\n")
    print(f"SCENARIO {code}: {res}")
    print(f"frames: {out}  (look at every one: index.txt says what each must show)")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
