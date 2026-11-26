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

## Done in pass 1b (tool = resave, one copy of the steps; unbuilt)
- Core/Serialize/SceneUpgrade.{hpp,cpp}: + the asset-reference chain (kAssetRefVersionReflected, MigrateReflectedAssetRefsV0ToV1
  over a MaterialGuidLookup - the step opens no file) and the light chain (kLightVersionRotation, MigrateLightRotationV0ToV1);
  floors AssetRef/Light = 0; UpgradeSceneRecords(..., materialGuid) runs them after WP; ContentMaterialGuidLookup()
  (PathForStableKey + Assets::ReadTextHeaderGuid, VFS-first) is the loader's lookup (UpgradeScene, ParseLoadablePrefab);
  DescribeUpgrade names asset refs / lights when they moved.
- Engine/Geometry/LightTravel.hpp (GeometryCore, NEW): kLightForwardAxis + RotationForTravel moved from ECS::Rules
  (SystemRules.hpp now `using Geometry::...`), so Core reaches the rule without including the ECS.
- Tools/SceneMigrator: the bodies of v72..v77 + AREF v0->v1 + light v0->v1 DELETED; SceneMigration.hpp names them
  `using Core::...` (tests spell Desert::Migration::X); FileMigrationReport holds `Core::SceneUpgradeReport Upgrade`
  (the moved flags/reports gone); RunSteps lost v72..v77, MigrateScene/MigratePrefab lost the v76 spline block and the
  v71 flag, RunWorldPartitionSteps lost v13/v14, RunAssetRefSteps/RunLightSteps -> RunEngineUpgrade (from = max(stated,
  floor) per tag; AssetsRootMaterialGuidLookup(assetsRoot) = the tool's old file locate+header read). MigratorMain prints
  report.Upgrade.X.
- Comments: SceneFormat.hpp (bumping rule, v73..v77, AREF/LGHT), RenderMethods.hpp, PostProcessSettings.hpp.

## TODO pass 2
1. BUILD (Editor + SceneMigrator + Tools tests): expected spots - SceneUpgrade.cpp includes (glm eulerAngles,
   <filesystem>), `Geometry::` lookup inside ECS::Rules (if ambiguous: `::Desert::Geometry::`), std::max in
   RunEngineUpgrade (<algorithm>), unused helpers/includes left in SceneMigration.cpp (<fstream>/<sstream> still used by
   AssetsRootMaterialGuidLookup).
2. TMLN v1 -> v2 (UIAnim) NOT moved: Core sits below AnimationCore/Engine (cmake/DesertModuleTable.cmake row Core), so the
   step cannot be in SceneUpgrade. Chosen design (UE: a struct's Serialize reads its own custom version): the TIMELINE
   READER upgrades it - move Tools/SceneMigrator/Source/ClipInterpShift.{hpp,cpp} (pure: Sequence + Json) to
   Engine/Animation/Timeline/TimelineInterpShift.{hpp,cpp} (namespace Animation::Timeline; add the name to the Engine row
   `Timeline/(SequenceFormat|TimelineInterpShift)\.` in cmake/DesertModuleTable.cmake:93, drop
   Desert/Tests/CMakeLists.txt:102), and in SequenceFormat.cpp ReadSequence (:641) replace the "run SceneMigrator"
   refusal of v1 with ShiftTimelineV1 (proved by VerifyInterpShift; a failed proof stays a refusal). Then the tool's
   MigrateUIAnimationTimelinesV1ToV2 (SceneMigration.cpp ~1870, RunSteps "TMLN v1 -> v2") becomes ReadSequence ->
   WriteSequence, i.e. the loader's function; fix includes in ClipLift.cpp, ClipMigration.cpp, MigratorMain.cpp,
   SceneMigration.cpp/.hpp, the two tests (timeline_lift_test, clip_timeline_migration_test).
3. Scenario: GI_Bistro_Night.desce v71 -> log "upgraded v71->v77 in memory"; `SceneMigrator --check` on
   Projects/Desert/Content/Scenes reports the same raise (report.Upgrade from Core::UpgradeSceneRecords).
4. Tests SceneVersionGate / PrefabVersionGate (Desert/Tests/Engine) still assert the old refusal of older files; update
   when tests are re-enabled (keep compiling).
