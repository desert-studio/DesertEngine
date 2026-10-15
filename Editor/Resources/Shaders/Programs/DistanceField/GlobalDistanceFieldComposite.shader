// DesertAsset {"Kind":"Shader","Guid":"f84db15352c7451890d5593b07157669","Versions":{"SHDR":1},"Dependencies":[]}
// THE GLOBAL DISTANCE FIELD'S UPDATE: one 8^3 workgroup per update brick of one clipmap, every voxel the minimum
// over the objects culled to that brick of their mesh distance field, clamped to the influence radius. Ported from
// UE 5.8 Engine/Shaders/Private/DistanceField/GlobalDistanceFieldCompositeObjects.usf (the per-voxel min over the
// culled object list), adapted: the brick's object list arrives from the CPU cull (GlobalDistanceField::Update)
// instead of a GPU culling grid, the output is a dense toroidal clipmap instead of a page atlas.
// THE OBJECT GRID in the same pass (UE CompositeObjectsIntoObjectGridPagesCS, GlobalDistanceFieldObjectGrid.ush):
// every cell of two voxels lists the four instance-table rows whose mesh surface lies nearest its centre, within the
// card interpolation range, sorted nearest first (GlobalDistanceField.hpp, THE OBJECT GRID). The brick's object list
// already holds them: it is culled with the four-voxel influence radius, a cell asks for less than that.
Shader "GlobalDistanceFieldComposite"
{
    Compute
    {
        LocalSize(8, 8, 8);

        Uniform(0) sampler3D u_DFBrickAtlas;
        ReadBuffer(1) DFAssetData
        {
            vec4 u_DFAssetData[];
        };
        ReadBuffer(2) DFIndirection
        {
            uint u_DFIndirection[];
        };
        ReadBuffer(3) DFObjects
        {
            vec4 u_DFObjects[];
        };
        // Per update brick: (brick x, y, z in world brick units, offset of its list in u_BrickObjects).
        ReadBuffer(4) GDFUpdateBricks
        {
            ivec4 u_UpdateBricks[];
        };
        // Per list: the object count, then that many object indices.
        ReadBuffer(5) GDFBrickObjects
        {
            uint u_BrickObjects[];
        };
        layout(binding = 6, r32f) restrict writeonly uniform image3D u_Clipmap;
        // Per object of u_DFObjects: its instance-table row, or GDF_OBJECT_GRID_INVALID (no cards).
        ReadBuffer(7) GDFObjectRows
        {
            uint u_ObjectRows[];
        };
        layout(binding = 8, r32ui) restrict writeonly uniform uimage3D u_ObjectGrid;

        PushConstant GDFCompositePush
        {
            uint  u_FirstBrick;      // this clipmap's first entry in u_UpdateBricks
            float u_VoxelSize;       // centimetres
            float u_InfluenceRadius; // centimetres
            uint  u_Level;           // the object grid's slab of this clipmap
        };

        #include <Common/MeshDistanceField.glslh>

        #define GDF_OBJECT_GRID_INVALID 0xFFFFFFFFu

        // UE PackObjectGridCellIndex: the row under its distance quantised to 8 bits, so a smaller word is nearer.
        uint PackObjectGridSlot(uint row, float distance, float cellExtent, float maxQuery)
        {
            const float normalized = clamp((distance + 0.25 * cellExtent) / (maxQuery + 0.25 * cellExtent), 0.0, 1.0);
            const uint  quantised  = uint(clamp(normalized * 256.0, 0.0, 255.0));
            return (row & 0xFFFFFFu) | (quantised << 24u);
        }

        // UE AddToObjectGridCell: replaces the farthest of the four when the new one is nearer.
        void AddToObjectGridCell(inout uvec4 cell, uint packed)
        {
            const uint farthest = max(max(cell.x, cell.y), max(cell.z, cell.w));
            if (packed >= farthest)
                return;
            for (int i = 0; i < GDF_OBJECT_GRID_SLOTS; ++i)
                if (cell[i] == farthest)
                {
                    cell[i] = packed;
                    return;
                }
        }

        // The cells of the brick: one invocation per cell (the even voxel of each 2^3 block) lists its rows.
        void CompositeObjectGridCell(ivec4 brick, uint listOffset, uint count)
        {
            if (any(notEqual(gl_LocalInvocationID % uint(GDF_OBJECT_GRID_CELL_VOXELS), uvec3(0u))))
                return;
            const ivec3 cell       = brick.xyz * (GDF_UPDATE_BRICK / GDF_OBJECT_GRID_CELL_VOXELS) +
                                     ivec3(gl_LocalInvocationID) / GDF_OBJECT_GRID_CELL_VOXELS;
            const float cellSize   = float(GDF_OBJECT_GRID_CELL_VOXELS) * u_VoxelSize;
            const vec3  centre     = (vec3(cell) + 0.5) * cellSize;
            const float cellExtent = 0.5 * cellSize;
            const float range      = GDF_OBJECT_GRID_INTERPOLATION_VOXELS * u_VoxelSize;
            // UE MaxQueryDistance: the cell's half diagonal (1.44 of its half size) plus the interpolation range.
            const float maxQuery   = 1.44 * cellExtent + range;
            uvec4 slots = uvec4(GDF_OBJECT_GRID_INVALID);
            for (uint i = 0u; i < count; ++i)
            {
                const uint objectIndex = u_BrickObjects[listOffset + 1u + i];
                const uint row         = u_ObjectRows[objectIndex];
                if (row == GDF_OBJECT_GRID_INVALID)
                    continue;
                const float distance = DistanceToNearestSurfaceForObject(objectIndex, centre, maxQuery);
                if (abs(distance) < maxQuery)
                    AddToObjectGridCell(slots, PackObjectGridSlot(row, distance, cellExtent, maxQuery));
            }
            // UE SortObjectGridCell: nearest first, the invalid words last.
            for (int pass = 0; pass < GDF_OBJECT_GRID_SLOTS - 1; ++pass)
                for (int i = 0; i < GDF_OBJECT_GRID_SLOTS - pass - 1; ++i)
                {
                    const uint a = min(slots[i], slots[i + 1]);
                    const uint b = max(slots[i], slots[i + 1]);
                    slots[i]     = a;
                    slots[i + 1] = b;
                }
            const ivec3 wrapped = ((cell % GDF_OBJECT_GRID_RESOLUTION) + GDF_OBJECT_GRID_RESOLUTION) %
                                  GDF_OBJECT_GRID_RESOLUTION;
            const ivec3 base = ivec3(wrapped.x * GDF_OBJECT_GRID_SLOTS, wrapped.y,
                                     int(u_Level) * GDF_OBJECT_GRID_RESOLUTION + wrapped.z);
            for (int i = 0; i < GDF_OBJECT_GRID_SLOTS; ++i)
                imageStore(u_ObjectGrid, base + ivec3(i, 0, 0), uvec4(slots[i]));
        }

        void main()
        {
            const ivec4 brick = u_UpdateBricks[u_FirstBrick + gl_WorkGroupID.x];
            const ivec3 voxel = brick.xyz * GDF_UPDATE_BRICK + ivec3(gl_LocalInvocationID);
            const vec3  world = (vec3(voxel) + 0.5) * u_VoxelSize;

            float minDistance = u_InfluenceRadius;
            const uint listOffset = uint(brick.w);
            const uint count = u_BrickObjects[listOffset];
            for (uint i = 0u; i < count; ++i)
            {
                const uint objectIndex = u_BrickObjects[listOffset + 1u + i];
                minDistance = min(minDistance, DistanceToNearestSurfaceForObject(objectIndex, world, minDistance));
            }
            const ivec3 texel = ((voxel % GDF_RESOLUTION) + GDF_RESOLUTION) % GDF_RESOLUTION;
            imageStore(u_Clipmap, texel, vec4(clamp(minDistance, -u_InfluenceRadius, u_InfluenceRadius)));
            CompositeObjectGridCell(brick, listOffset, count);
        }
    }
}
