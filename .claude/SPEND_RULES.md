# Spend rules (owner 11-27, after NG6-DEV burned 4 % of the week in 2 h and the PC crashed)

The lead decides the cheapest path before every launch. A hook enforces the rules marked [hook].

| Situation | Do | Never |
|---|---|---|
| Branch reconcile, mass conflicts | lead: merge commit + one build alone, then fix per real compile error | fan agents out over a conflict table |
| Mechanical work (merge, sources_sync, setup, census update) | lead runs a script | an agent |
| Engineer package | estimated read volume <= 250k chars of owned files; the brief names files and lines [hook] | a package over budget |
| Run hit its context limit | stop; split it or switch to build-error fixing | a "remainder" rerun of the same shape [hook] |
| Build | alone on the machine, -j8, no agents running [hook] | build beside agents or a second build |
| Waiting | one background notification | polling, idle wakes, sleep loops |
| Search | haiku Explore or grep digest (<= 60 lines) | whole-file reads into the lead |
| Agent count | 2 by default; more only for small disjoint packages | filling slots because they are free |
| Brief to an agent | spend conditions copied in: read budget, no whole-file reads of files > 2000 lines, stop at 70 % context with a commit | an open-ended brief |
