# NG6-DEV-B — remainder (context limit reached)

Brief: C:/Users/danil/.claude/studio/briefs/NG6-DEV-B-BRIEF.md; design F:/aftmp/NG6-DEV-DESIGN.md («### Пакет B», D11–D16).

## Done (commits f913a770c, 383828e33)
- D11/D12: `Core/SceneUpgrade/Steps/PostProcess1Bloom.{hpp,cpp}` (kPostProcessVersionBloom, PostProcessBloomReport,
  MigratePostProcessBloomV0ToV1 = dev body on StepEdits.hpp, RunPostProcessBloom); `UpgradeChain::PostProcess`
  (SceneUpgradeSteps.hpp), row + kChainOrder last + static_assert (SceneUpgradeSteps.cpp); kPostProcessUpgradeFloor,
  UpgradeFloorOf case, SceneGenerations::PostProcess (SceneUpgrade.hpp); StatedGenerations / CurrentGenerations /
  newer+floor refusals / GenerationsText / DescribeUpgrade "post process v{}->v{}" (SceneUpgrade.cpp).
- D14: `Engine/Generated/` absent in the tree; no package file includes a `.gen.*`.
- D15: ComponentSchema.lock = dev + no `Settings.AutoExposureKey`, `[WaterBody] Version 3` (no TargetWaveMaskDepth /
  FallFoam, + FallSound) as ng6build.
- D16: PrefabFormat.cpp = dev + `AssetEnvelope.hpp`, `ContentKinds.hpp` includes, include
  `Engine/Core/SceneUpgrade/SceneUpgrade.hpp`.

## Left
1. `Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.cpp:25` — the string line is > 120 columns after the
   path rename; reflow the literal (no text change).
2. D13 `Tools/SceneMigrator/Source/*` (merge took ng6build side). Bring dev's delta
   (`git diff e948e78ed origin/dev -- Tools/SceneMigrator/Source/{MigratorMain.cpp,SceneMigration.cpp,SceneMigration.hpp}`)
   into the ng6build text:
   - SceneMigration.hpp (~line 583, the `using Core::` block of the asset-ref/light chains): add
     `using Core::kPostProcessVersionBloom; using Core::MigratePostProcessBloomV0ToV1; using Core::PostProcessBloomReport;`
     and the comment "ASSET-REFERENCE, LIGHT AND POST-PROCESS CHAINS". The include must be
     `Engine/Core/SceneUpgrade/Steps/PostProcess1Bloom.hpp` if the header includes step headers one by one (check how
     Light1Rotation.hpp reaches it).
   - SceneMigration.hpp: add dev's `MigrateAnimGraphV4ToV5`, `AnimGraphAxesRaise`, `MigrateAnimGraphV5ToV6`
     declarations (ANGR 6, ANIM-BS2D-RUNTIME) and their bodies from dev's SceneMigration.cpp; dev's PPRO 1 for text
     assets / prefabs and SEQ-BONE-KEYS parts of SceneMigration.cpp (same diff).
   - MigratorMain.cpp: ng6build reports steps through `report.Upgrade.Steps` (StepOutcome list, no `*Raised` flags)
     — the dev "post process v{}->v{}" print (dev line ~656, `PostProcessBloomRaised`) must NOT come back as a flag;
     check that the generic Steps print covers it (it names the row "PostProcessBloom" with its Summary). Dev's ANGR
     4/5 -> 6 branch in the `.danimgraph` loop and the `tag == "ANGR" ? Desert::Assets::kAnimGraphSchemaVersion`
     TargetSkeleton raise go in as dev wrote them. Any header stamp that lists tags must include PPRO.
   - New dev source files under Tools/SceneMigrator/Source/ (if any): add to its committed `Sources.cmake`.
3. NEEDS (comment paths, other owners): `Core/Serialize/SceneFormat.hpp:176`, `ECS/PostProcessVolumeComponent.hpp:49`
   → `Core/SceneUpgrade/Steps/PostProcess1Bloom.hpp`; `Core/PostProcessSettings.hpp:63` (package C) →
   `Core/SceneUpgrade/Steps/V75GrassRasterPath.hpp`.
4. Then: arch_census, commit «wip: NG6-DEV-B код готов», push, the reviewer chain of the brief, `studio.py accept`.
