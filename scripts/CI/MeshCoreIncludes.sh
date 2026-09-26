#!/usr/bin/env bash
# ONLY THE GEOMETRY ADAPTERS MAY INCLUDE Engine/Geometry/MeshCore/ FROM OUTSIDE IT.
#
# WHY THIS EXISTS. MeshCore is UE's GeometryCore ported into the project (GC1-GC5). The rest of the engine and
# the editor reach it through a thin layer of adapters in Engine/Geometry/ (EditMeshBridge and the DynamicMesh*
# files), so a change of the port's API is absorbed in one place. Before GC5 the editor's MeshElementSelection
# included two MeshCore headers directly and nothing said so.
#
# THE RELATION PINNED HERE is not a count. It is: every tracked source outside MeshCore/ and outside the test
# suites that includes MeshCore/ is a NAMED row of ALLOW below, each with its reason -- and every row still
# includes MeshCore/ (a row that no longer does is a stale register and fails too, so the list cannot rot into
# a blanket permission). The file list comes from `git ls-files`, not the disk.
set -u
cd "$(dirname "$0")/../.." || exit 2

G=Desert/Desert/Source/Engine/Geometry
# path | reason
ALLOW=(
    "$G/EditMeshBridge.hpp|the bridge between EditMesh and DynamicMesh3: its API converts one into the other"
    "$G/DynamicMeshAsset.hpp|adapter: the asset holds a DynamicMesh3"
    "$G/DynamicMeshAsset.cpp|adapter: reads and writes the DynamicMesh3 attribute set"
    "$G/DynamicMeshRenderConversion.hpp|adapter: DynamicMesh3 -> render mesh"
    "$G/DynamicMeshRenderConversion.cpp|adapter: walks the DynamicMesh3 attribute overlays"
    "$G/DynamicMeshSerialization.hpp|adapter: DynamicMesh3 on disk"
    "$G/DynamicMeshSerialization.cpp|adapter: serializes the DynamicMesh3 attribute set"
    "$G/DynamicMeshSelection.hpp|adapter: the editor's door to DynamicMesh3 + GroupTopology (MeshElementSelection holds both)"
    "$G/MeshRegionOperation.hpp|adapter: Extrude/Inset/Outset on a DynamicMesh3 region"
    "$G/MeshRegionOperation.cpp|adapter: drives the ported OffsetMeshRegion/InsetMeshRegion and their helpers"
)

status=0
hits=$(git ls-files -- 'Desert/*' 'Editor/*' 'Runtime/*' 'Tools/*' \
         | grep -E '\.(cpp|hpp|h|inl)$' \
         | grep -v "^$G/MeshCore/" | grep -v '^Desert/Tests/' \
         | xargs grep -lE '#include[[:space:]]*[<"]Engine/Geometry/MeshCore/' 2>/dev/null)

for f in $hits; do
    allowed=0
    for row in "${ALLOW[@]}"; do [ "${row%%|*}" = "$f" ] && allowed=1; done
    if [ "$allowed" = 0 ]; then
        echo "MESHCORE INCLUDED OUTSIDE THE ADAPTERS: $f"
        grep -nE '#include[[:space:]]*[<"]Engine/Geometry/MeshCore/' "$f" | sed 's/^/  /'
        status=1
    fi
done
[ "$status" = 1 ] && echo "  Go through Engine/Geometry/EditMeshBridge.hpp or a DynamicMesh* adapter, or add a named row with its reason to ALLOW in $0."

for row in "${ALLOW[@]}"; do
    f="${row%%|*}"
    if ! grep -qE '#include[[:space:]]*[<"]Engine/Geometry/MeshCore/' "$f" 2>/dev/null; then
        echo "STALE ALLOW ROW: $f no longer includes MeshCore/ (or is gone) -- delete its row."
        status=1
    fi
done

[ "$status" = 0 ] && echo "meshcore includes: OK (${#ALLOW[@]} allow-listed adapter(s), tests and MeshCore/ itself exempt)"
exit "$status"
