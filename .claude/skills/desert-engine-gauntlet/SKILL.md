---
name: desert-engine-gauntlet
description: The long polish loop of DesertEngine — one artifact, one standard, many small blind-judged rounds with a ledger that survives sessions (scripts/gauntlet.py). Use when the owner asks to polish a look against a reference over many rounds, when the same artifact has been "improved" three times without converging, or when a quality bar must be held across sessions.
---
# The gauntlet loop

## Building is fan-out, polishing is a loop (owner, 2026-08-28)
- **Building** — a seam, a panel, a migration, a new capability: parallel packages over disjoint files, skill `desert-engine-feature`.
- **Polishing** — one artifact, one standard, many small rounds: this skill.

Deciding wrongly is the one setup error later rounds cannot repair: a loop applied to something not yet built polishes whatever happens to exist; fan-out applied to a polish problem gives four people improving four things against no common standard. A look complaint whose causes are known is a FEATURE (one design over all of them); the loop is for what remains when every named difference is closed and the frame still loses to the reference.

## The bar
A bar must be openable and subject-matched. A reference frame of a different subject (a distant thin sheet against a near deck) cannot decide anything about shape; a loop against it industrialises a comparison already known to be wrong. Before round 1, write in `BAR.md` what the bar can and cannot decide. The reference comes from the skill `desert-engine-look`.

## A round
Everything lives in `.gauntlet/<slug>/`; `scripts/gauntlet.py` of this skill creates and parses it. Hand-written ledger files are where runs break.

```bash
S=.claude/skills/desert-engine-gauntlet/scripts/gauntlet.py
python $S init --slug falls-foam --goal "the foot of the fall reads as foam, not paint" \
  --bar "<reference frame> — what it can and cannot decide: BAR.md" \
  --capture "python .claude/tools/play_scenario.py <TREE> Scenarios/Water/FALLS-FOAM.scn"
python $S next --slug falls-foam
```

1. **Gates first**: `python scripts/Dev/arch_census.py`; the round's code is in a built batch (the lead builds).
2. **Capture** by the scenario, with the rules of `desert-engine-verify` («Believing a frame»): measured floor, first render discarded, every frame checked against its log.
3. **Pair blind**: `pair --ours <png> --ref <png>` shuffles them into `ab/A` and `ab/B`. Do not open `key.json`.
4. **Verdict first**: write `rounds/NN/verdict.md` judging A against B through this round's lens. `reveal` refuses until it exists and is non-empty.
5. **Reveal**, and record what you believed. A lens that mispredicts twice needs rewording: that is a finding about the rubric, not about the work.
6. **One change.** Everything else noticed goes to `open_gaps.md` with the round it was seen in. One gap per round keeps rounds cheap enough to compound.
7. **Log it**: `log`.

What the protocol guarantees: `reveal` records the verdict's sha256 and `audit` recomputes it, so a verdict edited to fit the answer is detectable. It is a commitment with an audit trail, not a sealed envelope.

## A round may close with a refusal
`--outcome refused` closes a round, and it is a completed round. The rules of a refusal are in `desert-engine-contract` («A measured refusal counts as done»). A loop with no exit spends the night on a change that measures at six tenths of a grey level.

## Rotating the lens
One critic per round; `next` picks the lens from `RUBRIC.md` (default: silhouette, light, scale, coherence). Each lens judges a version already hardened by the previous one. Lenses are only tightened: a rubric relaxed so that the current build passes has stopped being a standard. Coherence exists because a change that looked like real granularity at one elevation grew a lattice at another: judge all the scenario's frames, not the one that flatters.

## What goes in git
The ledger only: `STATE.md`, `BAR.md`, `RUBRIC.md`, the verdicts and the reveal receipts. Frames are never committed; they stay under the scratch directory and the two or three that carry the argument go to the owner directly.

## One day
A loop is not a licence to stretch a task: the rounds of one day land that day as one batch, and the ledger is what carries the standard to the next session — not an unlanded branch.
