# N9 — the HDR skybox has no material and no preview

Read `.claude/AGENT_BRIEF_COMMON.md` first. Your tree:
`/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-N9`, branch `task/N9-sky`.
Absolute paths ALWAYS.

## Start by reading what your predecessor proved

`Docs/Sky/N7_HANDOVER.md`, and the two commits it names (`1d14f3cf`, `82e06b71`), already merged
into `dev`. That agent died on an API error, not on a mistake. **Its finding is load-bearing for
you**: the green-face-everywhere defect was a missing perspective divide in the view-ray
reconstruction, now fixed and held by `Tests/Engine/DepthConvention` (14/14, I re-ran it).
Evidence frames: `/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-N7-evidence`.

Do not re-diagnose that. Build on it.

## The owner's two reports

1. «для hdr скайбокс нет материаллов (как для mesh) со своими параметрами, а так же там нельзя
   посмотреть preview на шаре (как в UE)» — a mesh has a material section with parameters and a
   sphere thumbnail; an HDR skybox has neither.
2. «Ну надо еще добавить какие можно, можно опять же подсмотреть на UE» — more sky parameters.

## Scope

**The preview.** Your predecessor had oriented itself at `AssetThumbnailRenderer`,
`ThumbnailSubject` and the thumbnail service, and had written nothing. Start there. The mesh
material preview already exists — find it and ask whether the sky can reuse it rather than
growing a second one.

**The parameters.** Candidates: yaw rotation, tint, cubemap resolution, lower-hemisphere colour,
volumetric contribution. These were already triaged once: five UE occlusion knobs,
`SkyDistanceThreshold`, `CastShadows`, `SourceType` and `AffectsWorld` were **explicitly refused
with reasons**. Find those reasons before resurrecting any of them.

UE is a reference to a PROBLEM, not a shape to copy. For each parameter say what question it
answers; if our mechanism answers it more simply, take ours and write down why. A reasoned
refusal is a full answer here — the owner has said so.

## Do not

Split the skybox into separate atmosphere/HDR components. The owner asked whether that is right
(«стоит ли делить»); it has no answer yet and it is not yours to decide inside this task.

## Proof

A parameter that is written but never read is the recurring defect in this codebase — find the
line that DOES the thing, do not trust a comment claiming it. For each parameter you add, show a
frame where changing it changes pixels, and a negative control where it must not. Measure the
noise floor **per region**, never over the whole window. Shoot zenith, mid and horizon.
`--shot-frames 3` is FORBIDDEN: below frames-in-flight `--shot` writes an EMPTY png and two
empties are byte-identical.

## Rules

Never run clang-tidy or clang-format. English commit messages. Own scratchpad subdirectory.
WIP-commit whenever you pause. Never commit ThirdParty pointers. Build at `-j3`.
