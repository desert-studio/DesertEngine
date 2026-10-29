---
name: desert-engine-gauntlet
description: >
  Run a long polish loop against a reference bar: capture, compare BLIND against the
  standard, close the single biggest gap, repeat — with the run's state on disk so it
  survives /clear, /compact and a dead session. Use when the work is "make this look
  like X", "close the gap to the reference", "keep going until it beats the bar",
  "loop on this overnight", or when a quality complaint ("it looks cartoonish", "the
  clouds sit too low") needs many cheap rounds rather than one big change. Also use
  when a previous run left a ledger in .gauntlet/ — resume it instead of re-deriving
  the bar. Do NOT use for building something new (that is fan-out work with file
  ownership, see desert-engine-contract §5), for a one-off fix with no standard to
  chase, or for an audit.
---

**Владелец 10-29 — два параметра всегда: АРХИТЕКТУРА И NEXTGEN; вид — из ассетов, код — только размещение/LOD** (DEV_CONTRACT §00b).


# The gauntlet loop, as this project runs it

A method for getting past "pretty good": name a standard the work probably cannot
reach, then loop — capture, compare, close the single biggest gap — until it wins or
the owner stops it.

**Method credit and licence.** The loop as published by studioigor (`game-exp-skills`),
and before it somethingbig.ai/gauntlet-loop after Matt Shumer's Claude of Duty. **That
repository carries no licence**, so none of its code or prose is reused here — this is
an independent implementation of a method, which is not copyrightable, and the credit
is a citation rather than a permission. Same treatment `Docs/LICENCE_RECORD.md` gives
Nubis: reimplement the formulas freely, cite the talk.

## Why this project needs it, specifically

Two failure modes have cost real time here, and neither is fixed by trying harder.

**The builder grades its own work.** Р9 produced a change every statistic endorsed —
2.8× the roughness for one multiply, fewer holes opened — and only the frame killed it:
at contrast 3 the near field grew the Alligator's own cell lattice as a bright web and
tonal contrast at EL 45 went *backwards*. Р0 wrote up the erosion-versus-march-chord
relation as its conclusion, with correct arithmetic on both sides, and its own render
refuted it. A verdict formed after the answer is known is not evidence.

**The run lives in a context.** Two sessions died mid-task on an API limit with 29 and
5 files uncommitted; `Docs/Sky/HANDOVER.md` records nineteen lost the same way earlier,
two agents in a row, the second one's transcript gone. A ledger on disk is the only
part of a run that survives the session that made it.

## Which shape of work this is — decide before starting

**Ruling of the product owner, 2026-08-28: building is fan-out, polishing is a loop.**

- **Building** — a seam, a panel, a migration, a new capability. Several developers in
  parallel worktrees with non-overlapping file ownership, as `desert-engine-contract` §5
  describes. M1–M3 and Р5 ran that way in one day with no conflict.
- **Polishing** — one artifact, one standard, many small rounds. This skill. Р0 → Р6 →
  Р9 was one polish loop cut into three tasks by hand, and each cut lost the ledger.

Getting this wrong is the one setup error later rounds cannot repair. A loop applied to
something not yet built polishes whatever happens to exist; fan-out applied to a polish
problem produces four people improving four different things against no common standard.

## The bar, and the one it must not be

A bar has to be **openable** and **subject-matched**. Ours has failed the second test:
`Docs/Clouds/UEReference/UE_mid.png` is a distant thin stratocumulus sheet while the
protocol sky is a near congestus deck. Р0 built four shape instruments and **refused all
four as evidence** for that reason; Р6 named a subject-matched reference as the single
thing that would change his conclusion.

So before round 1, write in `BAR.md` what the bar can and cannot decide. A loop against
a mismatched bar does not fail loudly — it industrialises a comparison already known to
be wrong, and forty rounds make that worse rather than better.

## A round

Everything lives in `.gauntlet/<slug>/`. `scripts/gauntlet.py` creates and parses it;
hand-writing the files is where runs break.

```bash
S=.claude/skills/desert-engine-gauntlet/scripts/gauntlet.py
python3 $S init --slug clouds-surface \
  --goal "the deck reads as cloud, not cotton wool" \
  --bar  "Docs/Clouds/UEReference/UE_mid.png — NOT subject-matched, see BAR.md" \
  --capture "scripts/MacOS/DomeSweep.sh Resources/Assets/Scenes/Clouds_Protocol.desce /tmp/dome"
python3 $S next --slug clouds-surface
```

1. **Free gates.** `make Desert config=debug -j8`, `make Editor config=debug -j8`, the
   full suite sweep, llvm@18 formatting on changed lines. They cost nothing and fail
   loudest. `desert-engine-contract` §3 is the list.
2. **Capture.** Per `desert-engine-verify` §1 — the **whole dome**, not six points.
   Measure the noise floor with a repeat shot; it is a property of the scene, not a
   constant. Discard the first render in a fresh worktree. Check every frame against
   its own log.
3. **Pair blind.** `pair --ours <png> --ref <png>` shuffles them into `ab/A` and `ab/B`.
   **Do not open `key.json`.**
4. **Verdict first.** Write `rounds/NN/verdict.md` judging A against B through this
   round's lens. `reveal` refuses until it exists and is non-empty.
5. **Reveal**, and record what you believed. A lens that mispredicts twice needs
   rewording, and that is a finding about the rubric rather than about the work.
6. **One change.** Everything else noticed goes to `open_gaps.md` with the round it was
   seen in. One gap per round is what makes rounds cheap enough to compound.
7. **Log it.**

**What the protocol actually guarantees.** `reveal` refuses without a verdict and
records the verdict's sha256; `audit` recomputes it, so a verdict edited to fit the
answer is *detectable*. It is a commitment protocol with an audit trail, not a sealed
envelope — anything on this filesystem can be read by whoever decides to. It makes
self-deception effortful and leaves a mark. Claiming more would be the same class of
lie the protocol exists to catch.

## A round may close with a refusal

**Ruling of the product owner, 2026-08-28: our measured refusal outranks "no permission
to stop at good enough".** The published method loops until the bar falls. Here,
`--outcome refused` closes a round, and it is a completed round rather than a failed one.

This project has accepted seven. Р6 measured a native-resolution trace at 1.3–3.4 % of
the gap for 2.83× the cost and refused the octave. Р9 refused the histogram contrast the
statistics endorsed. An earlier task built a 3D mip generator, measured it at six tenths
of a grey level, and removed it again. A loop with no exit would have spent the night on
each of them.

The rules that make it a refusal rather than a shrug are in `desert-engine-verify` §5b:
numbers not impressions, say what would change the answer, and record it where it will
be found.

## Rotating the lens

One critic per round, and the lens rotates — `next` picks it from `RUBRIC.md`. Each lens
judges a version already hardened by the previous one, which is worth more than several
lenses judging the same version.

Lenses are only ever **tightened**, never loosened. A rubric that relaxes to let the
current build pass has stopped being a standard.

The default rubric ships with silhouette / light / scale / coherence. **Coherence exists
because of a specific defect**: Р9's refused change looked like genuine granularity at
EL 25 and grew a lattice at EL 45, and a six-point protocol skipping EL 45 would have
shipped it. Judge the dome, not the angle that flatters.

## What goes in git

Commit the **ledger** — `STATE.md`, `BAR.md`, `RUBRIC.md`, the verdicts and the reveal
receipts. That is the part which survives, and it is small.

Do **not** commit every round's artifact. `Docs/Clouds/Shots` is already 508 MB of a
1.6 GB repository, and a dome sweep is 40 tiles per round; a full-history CI checkout has
already blown a ten-minute timeout once because of it (`Д4`). Commit the sheet and the two
or three frames that carry the argument, and say what you dropped.

## Related

- `desert-engine-verify` — what to capture and how to measure it honestly. It defines the
  capture; this skill defines how to judge the capture without fooling yourself. They
  compose: that one feeds this one.
- `desert-engine-contract` — what may not ship, the free gates, and the definition of done
  this loop's rounds are one instance of.
- `desert-engine-dev` — how the engine is built.
