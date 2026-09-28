# AF8 — remainder (part A done on task/AF8-cooked-tree)

Done: Saved/Cooked/<Platform> is the one cook output. PackageCook wipes it and stages the shipped DDC buckets
under `Cooked/Buckets/...`; GamePackager `StageShippedContent` stages every census tree under its pak key
(textures cooked without IMPT/SRCE, raw mesh sources dropped), the registry, the partitioned worlds and the
descriptor; `CollectCookedTree` packs that tree and nothing else. Saved/CookedAssets is gone;
AssetRegistryTool `cook` is gone (its GUID refusal moved into GatherShippedRegistry); `list` stays.

Cook inputs: project content through the .deproj (ASSETS_PATH = <projectDir>/<AssetsRoot>), the project's
Cooked/ tree (COOKED_PATH = <projectDir>/Cooked), and the engine runtime trees Resources/{Shaders,Fonts,Icons}
relative to the working directory (today that is Editor/Resources/ — the engine resources live in the editor
tree, and Starter's content lives in Editor/Resources/Assets: layout gap, lead cards moving it out).

## AF8b — Editor/Cooked/Meshes is authored content, not a cook (lead cards it)
- 16 tracked files (.skmesh x3, .skeleton x4, .anim x9) written by the skinned import (Editor/Source/Editor/Import/CookPaths.hpp:52,84,
  MeshDnD.cpp:56) and the Sequencer (Panels/Sequencer/SequencerPanel.cpp:310-312).
- ContentKinds.hpp:116-118 roots SkinnedMesh/Skeleton/Animation at MESH_PATH_COOKED; AssetHandle.hpp:93 has a "cooked"
  path root; Retarget.hpp:58 comments on it; BlendImporter.hpp:44,159 writes Cooked/BlendConvert (a temp, belongs in Intermediate/).
- Move to the Assets tree (content migration: keys `cooked:` -> `assets:` in scenes), then delete COOKED_PATH/MESH_PATH_COOKED
  as source roots and the {COOKED_PATH,"Cooked"} census row.

## AF8c — mesh source assets still ship IMPT/SRCE
- MeshSourceAsset.cpp:524-526 writes ImportInfo + Source; the stager copies them verbatim. A runtime cooked form like
  Assets::CookTextureAssetForRuntime (header + Meta + a key record, platform data in the DDC bucket) is needed; the new test
  `PackagedContent.TheArchiveIsTheCookedTreeAndNothingElse` (relation 3) will go red once a mesh asset is added to its fixture.

## Card's full target (not in this change)
- One cooked file per asset with the platform payload inside (UE .uasset/.ubulk); runtime without DDC code.
- Packaged-Runtime frame of Starter (FRAMES.md has no packaging recipe) — not taken; budget.
- Editor-only resources: Icons/Gizmo is now excluded (PackagedTree::EditorOnlySubtree); Splash/Branding are not in any packed tree.
  Still shipped though editor-only: Resources/Shaders Grid.shader (EditorGridPass.cpp:19) and DebugLine.shader
  (EditorColliderPass.cpp:66) — needs an editor-only shader list; the Gizmo assertion is vacuous in the temp-project fixture
  (no Resources/Icons there), so a fixture with the engine icon tree would make it bite.
