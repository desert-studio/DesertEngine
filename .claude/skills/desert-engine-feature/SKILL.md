---
name: desert-engine-feature
description: How a feature is delivered whole, in one day, in DesertEngine — one design, 1..4 parallel engineer packages over disjoint files behind a header contract, parallel review, one batch build, one acceptance, landing the same day. Use when starting any feature, a look complaint from the owner, an architecture rework, a feature too big for one run, or whenever a second "-2" pass of the same task is about to be launched.
---
# A feature in one day

The unit of work is the feature, never the fix and never the pass. Measured 2026-11-25: 106 features took three passes or more, median five; a pass was 14 minutes, the rest was slicing, re-reading and queues. Numbers named below live in `.claude/tools/limits.py`; the state lives in `~/.claude/studio/pipeline.json` and survives /compact (`python .claude/tools/studio.py digest`).

| Stage | Who | Command of the lead | Gate (who refuses) |
|---|---|---|---|
| design | `architect` | launch with the design path | hand-in hook: `## Порядок` must be a parallel plan |
| code | `engineer` × packages, all at once | `studio.py plan CODE DESIGN.md`, then its launch lines | agent_guard: planned package only, no file overlap with a live run |
| review | `reviewer` × packages, all at once | `studio.py accept CODE-A` per hand-in | ownership proof against the design; one fix run per `RETURN` |
| build | lead, all features of the batch | `studio.py merge CODE…`, `build_land.sh`, `studio.py built ok` | the scenario shoots every «Тогда [кадр]»; build_gate, arch_census |
| accept | `acceptance`, one editor session | `studio.py passed CODE` | every «Тогда [кадр]» PASS |
| land | lead, the same day | `land_dev.sh`, `studio.py landed CODE` | fast-forward, commit in dev; the design is archived and queued for the Wiki |

## A feature too big for one run
It is never run twice in a row and never spread over days. The architect cuts it by INTERFACE into `limits.MAX_PACKAGES` or fewer packages:
1. `### Контракт` — the shared headers, written in full next to the design (`<design>-contract/…`). They are the only way packages know each other.
2. `### Пакет A..D` — each with a `Файлы:` line (exact file, `Dir/`, glob) and a `Закрывает:` line (ids of `## Отличия`; every id has exactly one package). The sets are disjoint; a file two packages need becomes a contract header or goes whole to one of them.
3. `studio.py plan` commits the contract on `task/CODE`, makes one tree `S:/wt/CODE-A` and one brief per package, and prints the launch lines. All packages are launched in the same response.
4. Each engineer edits only its own files (the hook refuses the rest), reports `CONTRACT-BREAK:` or `NEEDS:` instead of reaching over, and pushes `task/CODE-A`.
5. Review runs per package in parallel; `studio.py merge` re-proves ownership and merges every package into the batch tree; then ONE build and ONE acceptance for the whole feature.

Still too big for `limits.MAX_PACKAGES` packages: two features, two designs, one shared contract, the same day. A `CONTRACT-BREAK` goes back to the architect, who changes the contract once for all packages; the affected packages get one fix run.

## Rules
- The owner's complaint is the scope. "Water looks bad" is one design over everything that makes water look bad.
- An unproven cause goes to an experiment BEFORE code (view mode, knock-out, number), not to a package.
- `-2` means the design was wrong: reopen the design, find the missing difference, run again with all of it. A third run goes to the owner with the reason.
- Owner 11-27: a package whose scenario passes and whose review names only cases outside the scenario is merged after the FIRST review; the rest becomes a card, not another fix pass (a third pass on NET-TELEPORT-ENCROACH-A cost ~2M units and changed nothing landed).
- One-defect fixes (`limits.FIX_BRIEF`) skip design and review: `fixer` with a fix brief (it runs its own reviewer and accept).
- At most `limits.RUNS_BEFORE_BUILD` hand-ins of one code before a build has seen it; `ACCEPT` with MINORs is closed by the author in one run and not reviewed again.
- At most `limits.FEATURES_IN_FLIGHT` features between `plan` and `landed`; one older than `limits.FEATURE_MAX_AGE_H` stops new plans until it lands or is dropped with a reason (`studio.py drop`).
- A feature has ONE board card; packages are lines in it. Card updates queue up and go to the `board` agent as one batch (`studio.py board`).
- While a build, a scenario run or a landing is in progress, nothing new starts on top of the code being built; the next feature's design and packages do.
