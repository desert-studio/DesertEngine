# REMAINDER — SCENE-UPGRADE-ON-LOAD pass 1 (branch task/SCENE-UPGRADE-ON-LOAD, worktree F:/wt/SCENE-UPGRADE)

Done in pass 1 (engine side, unbuilt):
- NEW Desert/Desert/Source/Engine/Core/Serialize/SceneUpgrade.{hpp,cpp} (Core module, cmake/DesertModuleTable.cmake Core row):
  constants kSceneVersion70..77, WP 12..14, floors (scene 70, WP 12, assetref = head 1, light = head 1);
  the six pure steps COPIED (frozen enum numbers) from Tools/SceneMigrator/Source/SceneMigration.cpp
  (TracedRequest 1482, ComposedGI 1569, Filmic 1631, GrassRaster 1686, LandscapeSplineEntities 552, TubeSourceLength 2415);
  UpgradeSceneRecords(entities, from, Scene|Prefab, name), UpgradeScene(SceneSerialized&), RestateCurrentGenerations,
  StatedGenerations/CurrentGenerations, DescribeUpgrade -> "'<src>': upgraded v71->v77 in memory, save to keep".
- SceneFormat.cpp ParseLoadableScene: upgrade instead of refuse; upgraded => Document rebuilt from the typed tree
  (else a dropped key returns as a foreign key on save); LoadableScene::Upgraded note; kMigratorPath removed.
- SceneSerializer.cpp Deserialize logs LoadableScene::Upgraded once.
- PrefabFormat.cpp ParseLoadablePrefab: UpgradeSceneRecords(Prefab) + restate header + LOG_INFO; kMigratorPath removed.

## TODO pass 2 (tool = pure resave, no own copy of the steps)
1. Tools/SceneMigrator/Source/SceneMigration.hpp:
   - delete kSceneVersionRootSiblingIndex(470)..kSceneVersionTubeSourceLength(531) + static_assert(533) and
     kWorldPartitionVersionHLODBuilderSettings/EntityHLODLayer/ImpostorLayers (632..649); add
     `#include <Engine/Core/Serialize/SceneUpgrade.hpp>` and `using Core::kSceneVersionX;` for each (MigratorMain + tests
     spell Desert::Migration::kSceneVersionX).
   - delete structs/decls TracedRequestReport..GrassRasterPathReport (1056-1102), LandscapeSplineEntitiesReport + decl
     (815-828), MigrateTubeSourceLengthV76ToV77 decl (760-762); `using Core::...` for them.
   - FileMigrationReport (1446): replace VolumetricFogRaised(1516), EntityHLODLayerRaised/ImpostorLayersRaised(1536-7),
     TracedRequest..TubeSourceLengthsStated (1578-1589) with `Core::SceneUpgradeReport Upgrade;`; AnyRaised (1610-1614):
     `|| Upgrade.AnyRaised()`.
2. SceneMigration.cpp: delete the six bodies (lines above); RunSteps (4912): delete the v72..v75 and v77 blocks
   (~5160-5215); MigrateScene (6251): delete the spline block and VolumetricFogRaised; RunWorldPartitionSteps: delete
   the v13/v14 flags; before MigrationHeader (both MigrateScene ~6449 and MigratePrefab ~6557) call
   `Core::UpgradeSceneRecords(entities, from', target, name)` with from' = max(stated, floor) per tag (the legacy chain
   just raised the file to the floors), Refused -> report.Refused.
3. MigratorMain.cpp 632-678: report.X -> report.Upgrade.X for the moved fields.
4. SceneFormat.hpp comments 130-147 / PostProcessSettings.hpp / RenderMethods.hpp / EngineComponentProvider.cpp name
   "Tools/SceneMigrator, MigrateX" — point them to SceneUpgrade.
5. UIAnimationTimelines TMLN v1->v2 (always-run, tool-only, Animation module): not in the in-memory chain; a >=v70
   file with a TMLN v1 UI clip would load unshifted — decide (move to engine Engine-module hook or prove none left).
6. Light v0->v1 (MigrateLightRotationV0ToV1, needs ECS::Rules::RotationForTravel from Engine module) and asset-ref
   v0->v1 (reads mesh files) stay tool-only => their floor = head; move light step if Core may include SystemRules.
7. Tests: Desert/Tests/Engine/SceneVersionGate, PrefabVersionGate assert the old refusal of OLDER files — now an
   older file in the window loads. Keep compiling; update when tests are re-enabled.
8. Build (Editor + SceneMigrator + tests compile) and the scenario: GI_Bistro_Night.desce v71 -> log
   "upgraded v71->v77 in memory", same frame; a v999 file -> refusal, nothing loaded.
