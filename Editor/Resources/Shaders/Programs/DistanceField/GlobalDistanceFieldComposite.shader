// DesertAsset {"Kind":"Shader","Guid":"f84db15352c7451890d5593b07157669","Versions":{"SHDR":1},"Dependencies":[]}
// THE GLOBAL DISTANCE FIELD'S UPDATE: one 8^3 workgroup per update brick of one clipmap, every voxel the minimum
// over the objects culled to that brick of their mesh distance field, clamped to the influence radius. Ported from
// UE 5.8 Engine/Shaders/Private/DistanceField/GlobalDistanceFieldCompositeObjects.usf (the per-voxel min over the
// culled object list), adapted: the brick's object list arrives from the CPU cull (GlobalDistanceField::Update)
// instead of a GPU culling grid, the output is a dense toroidal clipmap instead of a page atlas.
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

        PushConstant GDFCompositePush
        {
            uint  u_FirstBrick;      // this clipmap's first entry in u_UpdateBricks
            float u_VoxelSize;       // centimetres
            float u_InfluenceRadius; // centimetres
            uint  u_Pad;
        };

        #include <Common/MeshDistanceField.glslh>

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
        }
    }
}
