# BRIEF CG-SELECT (finish an unfinished card)

Owner 11-17: "закончим начатое". origin/task/CG-SELECT holds the 11-09 partial (unbuilt). A previous agent read it and wrote exactly what is left with file:line in F:/aftmp/REMAINDER-FINISH-EDITOR.md, section "## CG-SELECT" - that list is your scope.

## Архитектура
Эталон: UE Cube Grid tool (Modeling Mode): cell selection, push/pull, paint faces, rescale.
Варианты: (a) land the partial as is; (b) finish every remaining item on top of origin/task/FINISH-EDITOR.
Выбор: (b) - no flag or field that does nothing enters dev.

## Сделать
1. In your worktree: git fetch; reset --hard origin/task/FINISH-EDITOR; create task/CG-SELECT-2; merge origin/task/CG-SELECT; finish every item of its REMAINDER section (noexcept, errors as values, no try/catch/throw, no TODO, no stubs). Then also merge origin/task/ASSET-REFVIEWER and finish its section (UE Reference Viewer panel) in the same branch, if budget allows; else leave it unmerged.
2. Commit, push task/CG-SELECT-2 with git -C <worktree>. No build, no editor: the lead builds once.

## Сценарий
Дано: origin/task/FINISH-EDITOR plus origin/task/CG-SELECT.
Когда: the lead builds task/CG-SELECT-2 and runs Scenarios/*/CG-SELECT*.scn (write one if none exists, play_scenario format, see Scenarios/Foliage/FOLIAGE-TRACE.scn).
Тогда [build]: rc=0 errors=0 for Editor and DesertCtl.
Тогда [scenario]: PASS, every Тогда line checks the behaviour the card names.
Return: SHA, done items, anything refused with why.
