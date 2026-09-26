# MeshCore

Ported from Unreal Engine 5.8 GeometryCore/DynamicMesh (Epic licence record: waiver #UE-2026-0842), adapted to
the project's style: std/glm, project names, engine asserts; see plans GC1–GC5. Each file's header names the UE
source and line ranges it was ported from and what was adapted.

Only the Geometry adapters (`EditMeshBridge.hpp`, the `DynamicMesh*` files and `MeshRegionOperation` in
`Engine/Geometry/`) and the test suites include `MeshCore/` from outside this folder; everything else goes through
them. `scripts/CI/MeshCoreIncludes.sh` enforces it with a named allow-list.
