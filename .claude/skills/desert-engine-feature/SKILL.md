---
name: desert-engine-feature
description: How a feature is delivered whole, in one pass, in DesertEngine — design, one engineer run, review, one build, one acceptance. Use when starting any feature, a look complaint from the owner, an architecture rework, or whenever a second "-2" pass of the same task is about to be launched.
---
# A feature in one pass

Measured 2026-11-25: 106 features took three passes or more, median five, and 70 cards waited for acceptance. A pass was 14 minutes; the rest was slicing, re-reading and queues. So the unit of work is the feature, not the fix.

| Stage | Who | Output | Gate |
|---|---|---|---|
| 1 Design | `architect` | `F:/aftmp/<CODE>-DESIGN.md` | lead reads it: every difference has a proven cause |
| 2 Code | `engineer`, one run, one branch | `wip: … код готов` | agent_guard: no design, no launch |
| 3 Review | `reviewer`, has not seen the author's reasoning | `ACCEPT` / `RETURN` | the same engineer branch is fixed once |
| 4 Build | lead, all branches of the batch together | `build_land.sh`, rc 0 | build_gate, arch_census |
| 5 Acceptance | `acceptance`, the whole batch in one editor session | frame table | every «Тогда» PASS |
| 6 Land | lead, the same day | `land_dev.sh` | arch_census, fast-forward |

Stages 1–3 of different features run in parallel; stages 4–5 are one queue because there is one GPU and one editor.

## Rules
- The owner's complaint is the scope. "Water looks bad" is one design over everything that makes water look bad, not a list of fixes.
- An unproven cause goes to an experiment BEFORE code (view mode, knock-out, number), not to a pass.
- `-2` means the design was wrong. Do not launch it: reopen the design, find which difference was missing, then run again with all of it. A third run goes to the owner with the reason.
- Size: one engineer run is about 300 calls. Larger is split by INTERFACE (header contract first, owners of disjoint files), all landing together.
- One-defect fixes (≤ 2 items) skip stages 1 and 3 and stay in the small lane.
- A card is «Готово» only after stage 5; a card older than 48 h in «На проверке» goes first in the next editor session.
- At most 3 features in flight. Finish before starting.
