#!/usr/bin/env python3
"""Blind A/B rounds against a reference bar, with the run's state on disk.

WHY THIS EXISTS. Two failure modes cost this project real time, and neither is
fixed by trying harder:

  1. The builder grades its own work. R9 had a change every statistic endorsed --
     2.8x the roughness for one multiply -- and only the frame killed it. R0 wrote
     up a conclusion its own render then refuted. A verdict written after the
     answer is known is not evidence.
  2. The run lives in a context. Two sessions died mid-task on an API limit with
     29 and 5 files uncommitted; HANDOVER records nineteen lost the same way
     earlier. A ledger on disk survives /clear, /compact and a dead session.

WHAT IT GUARANTEES, EXACTLY. `reveal` refuses until a non-empty verdict exists,
and records the verdict's sha256 at reveal time. A later `audit` recomputes it,
so a verdict edited to fit the answer is DETECTABLE. It is a commitment protocol
with an audit trail, not a sealed envelope -- anything on this filesystem can be
read by anyone who decides to. It makes self-deception effortful and leaves a
mark; it does not make it impossible. Claiming otherwise would be the same class
of lie the protocol exists to catch.

Method credit: the gauntlet loop as published by studioigor (game-exp-skills) and,
before it, somethingbig.ai/gauntlet-loop after Matt Shumer's Claude of Duty. That
repository carries NO LICENCE, so none of its code or text is reused here; this is
an independent implementation of the method, which is not copyrightable. See
Docs/LICENCE_RECORD.md for how this project handles the same question elsewhere.

Standard library only. Python 3.9+.
"""

import argparse
import hashlib
import json
import random
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(".gauntlet")


def _now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(65536), b""):
            h.update(block)
    return h.hexdigest()


def _run_dir(slug: str) -> Path:
    d = ROOT / slug
    if not d.is_dir():
        sys.exit(f"no run '{slug}' -- expected {d}. Run `gauntlet.py init` first.")
    return d


def _round_dir(slug: str, number: int) -> Path:
    return _run_dir(slug) / "rounds" / f"{number:02d}"


def _state_rows(run: Path) -> list:
    """Data rows of the ledger table, header and separator excluded.

    The separator line begins `|-`, not `| `, so a filter on `"| "` keeps the header
    and drops nothing else — an off-by-one that made `next` re-offer a round already
    logged. Match any pipe line, reject the separator, then drop the one header.
    """
    state = run / "STATE.md"
    if not state.is_file():
        return []
    rows = [ln for ln in state.read_text().splitlines()
            if ln.startswith("|") and not ln.startswith("|-")]
    return rows[1:]


# ----------------------------------------------------------------- init


def cmd_init(args) -> None:
    run = ROOT / args.slug
    if run.exists():
        sys.exit(f"'{args.slug}' already exists. Use `next` to continue it.")
    (run / "rounds").mkdir(parents=True)
    (run / "ref").mkdir()

    (run / "BAR.md").write_text(
        f"# Bar -- {args.slug}\n\n"
        f"GOAL: {args.goal}\n"
        f"BAR: {args.bar}\n"
        f"CAPTURE: {args.capture}\n\n"
        "## Is this bar honest?\n\n"
        "A bar must be OPENABLE and SUBJECT-MATCHED. Ours has failed the second test before:\n"
        "the first cloud reference frame was a distant thin stratocumulus sheet, our\n"
        "protocol sky is a near congestus deck, and R0 refused four shape instruments as\n"
        "evidence for exactly that reason. A run against a mismatched bar industrialises a\n"
        "comparison we already know is wrong. If the bar is not subject-matched, say so here\n"
        "and say what it can and cannot decide.\n"
    )
    (run / "RUBRIC.md").write_text(
        "# Lenses -- one per round, rotating\n\n"
        "Format matters: `- **name** -- question`. `next` parses these.\n\n"
        "- **silhouette** -- does the edge read as a surface, or as a smooth blob?\n"
        "- **light** -- is there a gradient inside the body, and is shadow the right hue?\n"
        "- **scale** -- does the subject read at the size and distance it is supposed to be?\n"
        "- **coherence** -- does the whole dome agree with itself, or only the angle we shot?\n"
    )
    (run / "open_gaps.md").write_text(
        "# Noticed, not this round\n\n"
        "One gap per round. Everything else waits here with the round it was seen in.\n"
    )
    (run / "STATE.md").write_text(
        f"# {args.slug}\n\n"
        f"Started {_now()}. Goal: {args.goal}\n\n"
        "| round | lens | gap | outcome | note |\n"
        "|---|---|---|---|---|\n"
    )
    print(f"initialised {run}")
    print("Next: write the bar into BAR.md honestly, then `gauntlet.py next --slug " + args.slug + "`")


# ----------------------------------------------------------------- next


def cmd_next(args) -> None:
    run = _run_dir(args.slug)
    rows = _state_rows(run)
    number = len(rows) + 1

    lenses = [
        ln.split("**")[1]
        for ln in (run / "RUBRIC.md").read_text().splitlines()
        if ln.startswith("- **") and "**" in ln[4:]
    ]
    if not lenses:
        sys.exit("RUBRIC.md has no lenses in `- **name** -- question` form.")
    lens = lenses[(number - 1) % len(lenses)]

    bar = (run / "BAR.md").read_text()
    goal = next((l[5:].strip() for l in bar.splitlines() if l.startswith("GOAL:")), "?")
    capture = next((l[8:].strip() for l in bar.splitlines() if l.startswith("CAPTURE:")), "?")

    print(f"round {number:02d}   lens: {lens}")
    print(f"goal:    {goal}")
    print(f"capture: {capture}")
    print()
    print("1. Free gates first -- they cost nothing and they fail loudest:")
    print("   python scripts/Dev/arch_census.py -- the counts may only fall")
    print("   the round's code is in a batch the lead has built; an agent never builds")
    print("2. Capture the artifact with the command above.")
    print(f"3. gauntlet.py pair --slug {args.slug} --round {number} --ours <png> --ref <png>")
    print(f"4. Write rounds/{number:02d}/verdict.md judging A vs B through the '{lens}' lens.")
    print(f"5. gauntlet.py reveal --slug {args.slug} --round {number}")
    print("6. ONE change. Everything else goes to open_gaps.md.")
    print(f"7. gauntlet.py log --slug {args.slug} --round {number} --gap ... --outcome ...")
    print()
    print("A round may close as a RECORDED REFUSAL -- outcome `refused` -- when the")
    print("measurement says the change does not earn its cost. That is a completed round,")
    print("not a failed one. This project has accepted seven.")


# ----------------------------------------------------------------- pair


def cmd_pair(args) -> None:
    rd = _round_dir(args.slug, args.round)
    ab = rd / "ab"
    if (ab / "key.json").exists():
        sys.exit(f"round {args.round:02d} is already paired. Pairing twice would reshuffle a "
                 "comparison a verdict may already refer to.")
    ab.mkdir(parents=True, exist_ok=True)

    ours, ref = Path(args.ours), Path(args.ref)
    for p in (ours, ref):
        if not p.is_file():
            sys.exit(f"missing: {p}")

    # The shuffle is the whole point: the reader must not be able to infer which is which
    # from position. Seeded from the OS, not from anything reconstructible.
    swap = random.SystemRandom().random() < 0.5
    a_src, b_src = (ref, ours) if swap else (ours, ref)
    shutil.copy2(a_src, ab / ("A" + a_src.suffix))
    shutil.copy2(b_src, ab / ("B" + b_src.suffix))

    (ab / "key.json").write_text(json.dumps({
        "A": "reference" if swap else "ours",
        "B": "ours" if swap else "reference",
        "ours_path": str(ours), "ours_sha256": _sha256(ours),
        "ref_path": str(ref), "ref_sha256": _sha256(ref),
        "paired_at": _now(),
    }, indent=2) + "\n")

    print(f"paired into {ab}")
    print("A and B are shuffled. DO NOT OPEN key.json.")
    print(f"Write {rd/'verdict.md'} first, then `reveal`.")


# ----------------------------------------------------------------- reveal


def cmd_reveal(args) -> None:
    rd = _round_dir(args.slug, args.round)
    verdict, key = rd / "verdict.md", rd / "ab" / "key.json"
    if not key.is_file():
        sys.exit(f"round {args.round:02d} was never paired.")
    if not verdict.is_file() or not verdict.read_text().strip():
        sys.exit("REFUSED: no verdict yet. The verdict is written BEFORE the answer is read --\n"
                 "that ordering is the only thing making it evidence rather than a rationalisation.")

    receipt = rd / "reveal.json"
    if receipt.is_file():
        print("already revealed -- receipt below, key unchanged.\n")
    else:
        receipt.write_text(json.dumps({
            "revealed_at": _now(),
            "verdict_sha256": _sha256(verdict),
            "verdict_bytes": verdict.stat().st_size,
        }, indent=2) + "\n")

    print(json.dumps(json.loads(key.read_text()), indent=2))
    print("\nIf the verdict was wrong, that is a result: write down what you believed and why,")
    print("then look again. A lens that mispredicts twice is a lens that needs rewording.")


# ----------------------------------------------------------------- audit


def cmd_audit(args) -> None:
    run = _run_dir(args.slug)
    bad = 0
    for rd in sorted((run / "rounds").glob("*")):
        receipt, verdict = rd / "reveal.json", rd / "verdict.md"
        if not receipt.is_file():
            continue
        rec = json.loads(receipt.read_text())
        now = _sha256(verdict) if verdict.is_file() else "<missing>"
        ok = now == rec["verdict_sha256"]
        bad += 0 if ok else 1
        print(f"{rd.name}: {'ok' if ok else 'CHANGED AFTER REVEAL'}  revealed {rec['revealed_at']}")
    print("\nclean" if not bad else f"\n{bad} verdict(s) edited after the answer was read")
    sys.exit(1 if bad else 0)


# ----------------------------------------------------------------- log


def cmd_log(args) -> None:
    run = _run_dir(args.slug)
    state = run / "STATE.md"
    note = args.note or ""
    with state.open("a") as fh:
        fh.write(f"| {args.round:02d} | {args.lens} | {args.gap} | {args.outcome} | {note} |\n")
    print(f"logged round {args.round:02d}: {args.outcome}")
    if args.outcome == "refused":
        print("Recorded refusal. Say the number, say what would change the answer, and put it")
        print("where the next person will look -- the commit message or a comment at the site.")


# ----------------------------------------------------------------- main


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("init"); p.set_defaults(fn=cmd_init)
    p.add_argument("--slug", required=True)
    p.add_argument("--goal", required=True)
    p.add_argument("--bar", required=True, help="what the artifact loses to, and where it lives")
    p.add_argument("--capture", required=True, help="the exact command that produces the artifact")

    p = sub.add_parser("next"); p.set_defaults(fn=cmd_next)
    p.add_argument("--slug", required=True)

    p = sub.add_parser("pair"); p.set_defaults(fn=cmd_pair)
    p.add_argument("--slug", required=True)
    p.add_argument("--round", type=int, required=True)
    p.add_argument("--ours", required=True)
    p.add_argument("--ref", required=True)

    p = sub.add_parser("reveal"); p.set_defaults(fn=cmd_reveal)
    p.add_argument("--slug", required=True)
    p.add_argument("--round", type=int, required=True)

    p = sub.add_parser("audit"); p.set_defaults(fn=cmd_audit)
    p.add_argument("--slug", required=True)

    p = sub.add_parser("log"); p.set_defaults(fn=cmd_log)
    p.add_argument("--slug", required=True)
    p.add_argument("--round", type=int, required=True)
    p.add_argument("--lens", required=True)
    p.add_argument("--gap", required=True)
    p.add_argument("--outcome", required=True,
                   choices=["improved", "no-change", "regressed", "refused"])
    p.add_argument("--note", default="")

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
