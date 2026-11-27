# NG6-DEV-C — remainder (context limit hit)

Brief: C:/Users/danil/.claude/studio/briefs/NG6-DEV-C-BRIEF.md, design F:/aftmp/NG6-DEV-DESIGN.md («### Пакет C»).
Base e948e78ed, ng6build = 009d5f014 (ours), dev = df79e737d (theirs), contract merge 49fb8b442.

## Done (commit on task/NG6-DEV-C)
- D17 `AutoExposureKey` removed from dev's text: `Core/PostProcessSettings.hpp` (field + ng6 comments
  GpuCapabilities / `Core/SceneUpgrade/Steps/V75GrassRasterPath.hpp` / bias comment), `Graphic/ViewSettings.cpp`
  (Lerp line), `MaterialTonemap.{hpp,cpp}` (Params.ExposureKey gone, MPROPERTY MiddleGrey = kExposureMiddleGrey),
  `TonemapRenderer.cpp` (Params init), `AutoExposureRenderer.cpp:197` (ap.MiddleGrey = kExposureMiddleGrey),
  `BloomRenderer.hpp:86` (BloomFrame.ExposureKey gone), `BloomRenderer.cpp:171` (push.MiddleGrey),
  `Programs/Bloom/BloomSetup.shader` and `Composite/SceneComposite.shader` (ExposureKey -> MiddleGrey),
  `setting_consumers_test.cpp` (AutoExposureKey row gone + ng6build water rows moved to Desert/Water paths).
- Verified, nothing to carry: DecalRenderer.cpp / TranslucencyLightingVolume.cpp (OURS; dev's unity renames are
  already in ng6), AnimGraphPanelPoseGraph.cpp (THEIRS; RateScale moved by dev to BlendSpaceEdit.cpp),
  MeshImpostorBaking.cpp / WorldPartitionBuilder.cpp (identical both sides), AEAverage.shader (already MiddleGrey).

## Left
1. D18 `Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp` (merge kept ng6build). Carry dev's calls:
   `git diff e948e78ed df79e737d -- Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp` (54/44): bloom frame
   (BloomMethod, Gaussian stages, ConvolutionIntensity, BloomDirtMask*, threshold), AutoExposureSpeedUp/Down,
   AutoExposureLowPercent/HighPercent. Do NOT set any ExposureKey (BloomRenderer frame has none now). The
   consumers test expects these names read in SceneRenderer.cpp. File is in census list: growth < 100 lines.
   Check BloomRenderer/TonemapRenderer setters it calls (e.g. ng6 `bloom->SetAutoExposure(bool)` no longer
   exists in dev's BloomRenderer.hpp — dev uses the BloomFrame struct; `tonemap->SetAutoExposure(bool)` exists).
2. Auto-merged neighbours to read once against the new APIs: `SceneRendererFramePostFX.cpp`, `SceneRenderer.hpp`,
   `SceneRendererImpl.hpp` (grep `bloom`, `SetAutoExposure`, `GetExposureKey`).
3. D19: `Editor/Source/EditorLayer.cpp`, `Editor/Source/Editor/LevelEditor/ViewportCapture.{cpp,hpp}` are dev's
   text — carry `git diff e948e78ed 009d5f014 -- <file>` (12/13, 18/21, 6/3). `SceneWorkspace.cpp` is ng6build's
   text — carry `git diff e948e78ed df79e737d -- <file>` (17/6).
4. `python scripts/Dev/arch_census.py`, commit «wip: NG6-DEV-C код готов», reviewer, accept (brief «## Цепочка»).
