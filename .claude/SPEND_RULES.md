# Spend rules (owner 11-27, after NG6-DEV burned 4 % of the week in 2 h and the PC crashed)

The lead decides the cheapest path before every launch. A hook enforces the rules marked [hook].

| Situation | Do | Never |
|---|---|---|
| Branch reconcile, mass conflicts | lead: merge commit + one build alone, then fix per real compile error | fan agents out over a conflict table |
| Mechanical work (merge, sources_sync, setup, census update) | lead runs a script | an agent |
| Engineer package | estimated read volume <= 250k chars of owned files; the brief names files and lines [hook] | a package over budget |
| Run hit its context limit (100k) | the hook leaves 8 calls: commit, push, REMAINDER.md; a fresh agent continues from REMAINDER.md alone [hook] | working on past 100k; a second remainder of the same package without splitting it |
| Build | alone on the machine, -j8, no agents running [hook] | build beside agents or a second build |
| Waiting | one background notification | polling, idle wakes, sleep loops |
| Search | haiku Explore or grep digest (<= 60 lines) | whole-file reads into the lead |
| Agent count | 2 by default; more only for small disjoint packages | filling slots because they are free |
| Brief to an agent | spend conditions copied in: read budget, no whole-file reads of files > 2000 lines, at 100k context commit, push and write REMAINDER.md | an open-ended brief |
| Lead fixes a defect himself | before the build, write the defect arithmetic against the scenario statement: units (per frame / per repaint / per second), cold and warm run, the worst sample; build and accept only when the arithmetic closes it | a fix proven only by the next acceptance (SP-MODEL ETA: three acceptances, ~0.6M) |
| Acceptance | one scenario run per agent; a repeat or a second scenario is a fresh agent [hook, limits.ACCEPTANCE_RUNS]; report = the table, frames compared by numbers first, a frame opened once | two runs in one agent (88-95k of frames in context) |
| Lead context > limits threshold (55k) | handover into memory at the task boundary, then /clear typed by auto_compact.py [hook] | carrying a 60k+ lead context through more requests |
