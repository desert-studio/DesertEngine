# REMAINDER WP17 pass 5 (task/WP-LIVE) - corpus at v68, live verdicts DONE
- Fix: Tools/SceneMigrator/Source/SceneMigration.hpp:1042 Changed() lacked RootSiblingIndexRaised -> migrator said "already at v68" for v67 scenes and wrote nothing (dc7cf1c78). SceneMigrator only; Editor not rebuilt (Changed() not on the load path's behaviour).
- Corpus (67fcc8bf2): migrate.sh --write Projects Editor + scratch bundle -> 84 scenes + 3 prefabs SCNE 67->68 (stamp only; no in-repo partitioned root carried siblingIndex); 40 layout-only .defoliage/.demat re-lays restored to dev's bytes (as dcc7701b6). Bundle WP22_Grids raised in scratch.
- Live (bundle WP22_Grids, Editor @07af87b5e, DesertCtl; log scratch WP-LIVE/live5.txt): open -> modified=false; untouched Save Scene -> 0 files changed.
- Move Sun (selection Translation -30,-75,-55 -> 100,200,300) -> modified=true; save -> 2 files: 33/926003.deent (Sun) + Descriptors.dedesc (its descriptor row: bounds/cell, as UE's actor descriptor), modified=false.
- Undo -> modified=true; save -> the same 2 files rewritten; Sun .deent byte-identical to the original; whole bundle = original (0 differing files).
- VERDICTS: WP17 (untouched save writes nothing) PASS; WP17-B (edit saves only the edited entity's file + its descriptor row) PASS; WP17-C (undo marks dirty, save restores exact bytes) PASS.
- Suites for the lead: SceneVersionGate, PrefabVersionGate, WorldPartition, SceneStitch, WorldSceneGenerator. Mutation: drop `|| RootSiblingIndexRaised` in SceneMigration.hpp Changed() -> a v67 scene is reported ok and stays v67.
