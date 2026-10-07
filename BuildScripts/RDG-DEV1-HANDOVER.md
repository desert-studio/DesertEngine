# RDG-DEV1-B1 — handover to the dev merge (Windows lead, 2026-10-06)

Branch `task/RDG-DEV1-B1` = RDG-INT (4d5ec59f7) + origin/int/b2 (graph-form resolution, RDG-DEV1 9be5b2975..2aa60e324)
+ BUILD1-P0 (cd45fda08) + the two commits below. Final SHA: the commit that adds this file (parent a63392721).

- 8c9402d50 — five census reds after the dev merge (DefaultSurfacePipeline by role; EngineHost as a host with
  OpenSuiteProject + DerivedDataSandbox; ShippedShaderPasses reads every MeshRenderer*.cpp; capture-cut census follows
  ShotDirector; WorldTime census rows).
- a63392721 — ANIM-FIX1 rule change (owner decision, UE parity, accepted by desert-macos; Notion card updated):
  `WorldTime::EditorPreviewSeconds`, the editor animation preview freezes with the viewport's Realtime off.

## Gates (Windows, RTX 3070 Ti, MSBuild 17 / MSVC 14.44, P0 settings)

| config | SHA | SLN | suites |
|---|---|---|---|
| Debug | 68f9a28b8 | 0 errors | 384/389; the 5 reds fixed in 8c9402d50 and those 5 suites rerun green (rebuilt Desert + suites) |
| Debug | a63392721 | Desert + WorldTime + AnimatorPose rebuilt, 0 errors | WorldTime 14/14, AnimatorPose 11/11 (new relation test red under the old rule — mutation checked) |
| Release | 8c9402d50 | 0 errors (clean build, 4491 s) | **389/389** |
| Shipping | — | **not built** | pending |

Not rerun at the final SHA: the full Debug suite set (only the 7 touched suites ran after 68f9a28b8) and Release
for a63392721 (Scene.cpp, WorldTime.hpp, comments, two test files). The int branch CI covers both.

## Live checks

- Last clean live run: RDG-INT 4d5ec59f7 (pre-dev-merge) — m0 and m4: 0 VUID / 0 SYNC-HAZARD / 0 "did not execute",
  no black frames, frames byte-equal to the cc1d baseline except Clouds/Sky noise.
- **At B1 (post-dev-merge): PENDING — not run.** The Debug Editor rebuild for it was stopped for this handover.
  To do: m0 + m4 (machine.json default / MSAA 4x), glass, frames vs baseline, VUID/SYNC/did-not-execute/black.
  Expect frame differences from dev's own content changes (int/b2), so compare against a dev build, not the
  pre-merge baseline. Project path is now `--project ../Projects/Desert/Desert.deproj` (from `Editor/`).
- The baseline frames are stored locally on the Windows machine only (Claude scratchpad, session cc1d184a,
  `live/`). They are not in the repo, so a Mac-side merge cannot use them.

## Open issues / cards

- ShippingBoundary: a flake seen once in Debug (a directory vanished mid-walk), it passed on rerun. Follow-up.
- Gate run 1 at B1 showed 21 silent CL exit 1/4 + MSB4166. Run 2 (the one in the table) did not repeat them. Watch CI.
- Built on other branches, not merged here: FAULT1 (DE-8 @e05fcda9e, code complete, never built), SCAL1
  (c2a448678), BUILD1 P1 runners (86bc770a9), TAA1 (9c402177c). Each needs its first build + gate after this lands.
- `Reflection.gen.cpp`: DesertHeaderTool regenerates it with the include and registration order changed
  (order-only diff, reverted here). Do not commit that churn.

## BUILD1-P0 — what the merger must know (BuildScripts/BUILD1-CONTRACT.md §3, §6)

- Every generated `.vcxproj` gets `UseMultiToolTask=true` + `EnforceProcessCountAcrossBuilds=true`
  (`BuildScripts/MSBuildProperties.lua`, included from `Workspace.lua`). premake generation **fails** if a project
  with sources lacks either property, or if any project has `DebugInformationFormat=EditAndContinue`.
- Debug is `/Zi` (editandcontinue "Off"), still a full PDB and `/INCREMENTAL /DEBUG:FULL`. No FASTLINK: CrashHandler
  symbolisation depends on it. Release/Shipping flags are unchanged.
- `Ccache.props` keeps only CLToolExe/CLToolPath/TrackFileAccess, and the MultiToolTask settings moved out of it.
  `ci.yml` comments near the msbuild step were rewritten. Measured locally: Desert clean compile went from
  1810 s to 267 s.
- One P0 build spawns about 11 cl.exe processes, so run only one build at a time on a 12-core machine.

## Merge hazards against dev

- int/b2 adds only 9b2baca90 + fca6befab (ci.yml, Windows ccache restore/save and the 130-min build timeout).
  `git merge-tree` of this branch with origin/int/b2 and with origin/dev: **clean** (2026-10-06 evening).
- P0 edits `ci.yml` comments around the Windows msbuild step. The two CI commits add lines in the same job, but
  the merge is textually clean. Re-read that job after the merge.
- Behaviour change to check in the Mac editor: with Realtime off in the main viewport, an UpdateAnimationInEditor
  preview no longer moves (the animation editor's preview scene still does).
