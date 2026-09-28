# SPAWN1 — what is left (for the next agent)

Branch `task/SPAWN1-player-start`, wip commit. Code for SPAWN1a AND the view-target half of SPAWN1b is
written; **the engine/Editor/Runtime have NOT been compiled** (budget ran out). Only suite `PlayerStart`
(pure rules) and `StoredAssetForm` were run.

## Written (compile first: libDesert, then Editor, then Runtime)
- `Desert/Desert/Source/Engine/Core/PlayerStart.hpp` — PlayRequest, SpawnDefaultPawn, BeginPlay, ChoosePlayerStart, ChooseViewTarget.
- `Desert/Desert/Source/Engine/Core/PlayerStartRules.cpp` — the two pure rules (suite PlayerStart).
- `Desert/Desert/Source/Engine/Core/PlayerStart.cpp` — spawn (PrefabAsset::Instantiate at the start's world transform, yaw/pitch via glm::decompose) + BeginPlay (spawn → Scene::ResolveViewTarget → SetState(Play); rolls back on failure).
- `Components.hpp`: PlayerStartData/PlayerStartComponent (before ScriptComponent); CameraData::IsMainCamera → AutoActivateForPlayer (default false).
- `SceneSettings.hpp`: DefaultPawn (Asset<PrefabAsset>). `StoredAssetForm.cpp`: PrefabAsset → AssetsRelative; `ComponentRegistry.cpp`: PrefabAsset FromPath branch (find/create + load).
  NOTE: stored as an assets-relative PATH like every other AssetsRelative type, not {Guid,Path} as the brief said.
- `Scene.hpp/.cpp`: m_PlayerPawn / m_ViewTarget / m_PlayFromHere, ResolveViewTarget(), UpdateActiveCameraSource uses the resolved target; Clear() resets them; `r.prepare<PlayerStartComponent>()`.
- `ReflectedComponentBlocks.hpp`: "PlayerStart" block. `ComponentEditorRegistrations.cpp`: "Player Start" entry.
- `EditorLayer.cpp`: OnScenePlay(bool fromHere) — snapshot → BeginPlay → streamer (rollback on streamer refusal); toolbar chevron popup Play / Play from Here; palette/DesertCtl command "Play from Here".
- `LightGizmoRenderer.cpp`: PlayerStart billboard (SpawnPoint, green) + 1 m forward (-Z) arrow.
- `WorldSettingsPanel.cpp`: "Game Mode" section drawing DefaultPawn through PropertyEditorBuilder::DrawField; `PropertyEditorBuilder.cpp`: "PrefabAsset" picker (registry rows + drop).
- `RuntimeLayer.cpp`: both SetState(Play) sites → Core::BeginPlay. `UIRenderTextureCache.cpp`: ResolveViewTarget before Play.
- Rename IsMainCamera in: EditorLayer, AssetThumbnailRenderer, PhotogrammetryPanel, Tools/WorldGen/WorldBuild.cpp, 6 tests (WorldPartition*, WorldCells*, SettingConsumers).

## Not done
1. **Build + fix** libDesert (Reflection.gen.cpp regenerates), Editor, Runtime, WorldGen; run ComponentReflection, SettingConsumers, ConfigOwnership, PointerOwnership, AssetResolverCensus, PrefabInstantiationCensus (PlayerStart.cpp calls PrefabAsset::Instantiate, not PrefabFactory — should be fine), WorldPartition*, WorldCells*.
2. **Migration SCNE v40** (next free = 40): `Tools/SceneMigrator/Source/SceneMigration.hpp:152` add `kSceneVersionPlayerViewFlag = 40`, raise `Core::kSceneVersion` (`Engine/Core/Serialize/SceneFormat.hpp:63`), step in SceneMigration.cpp next to `MigrateUndeclaredKeysV38ToV39` (~:951): rename Camera key `IsMainCamera` → `AutoActivateForPlayer`; keep `true` only when the file has exactly ONE Camera block (the old default was true on every camera, so a missing key = true). 57 scenes in `Editor/Resources/Assets/Scenes/` + prefabs; `scripts/Dev/migrate.sh --write`. Tell the lead BEFORE raising the version.
   Until this lands, every existing scene loses the flag (unknown key / default false) → Play refuses with "no view for the player" unless a pawn with a camera is set. Headless `--play` is unaffected (pinned camera → ResolveViewTarget returns success).
3. `PlayRequest::PlayerStartTag` has no production caller yet (Runtime `--player-start <tag>` / DesertCtl arg) — wire it or drop it (dead-setting rule).
4. Test pawn prefab in the corpus (capsule StaticMesh + child Camera + CharacterController + controller script), Starter with a PlayerStart + DefaultPawn; frames: Play (pawn camera) via DesertCtl "Play", "Play from Here"; Stop → Save → `git diff` empty (the byte-identical proof; no suite compiles Scene.cpp, so it is proven live).
5. Mutations on the rules (e.g. drop the `untagged.size()==1` branch, flip the pawn-camera priority) — each must redden suite PlayerStart.
