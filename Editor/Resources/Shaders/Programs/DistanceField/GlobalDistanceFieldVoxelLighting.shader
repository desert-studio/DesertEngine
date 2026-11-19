// DesertAsset {"Kind":"Shader","Guid":"3c9e5b7a1d2f48e6a0b4c8d71f5e2a96","Versions":{"SHDR":1},"Dependencies":[]}
// THE GLOBAL DISTANCE FIELD'S VOXEL LIGHTING (FPS-GI-VOXEL-LIGHT; UE Lumen's voxel lighting, LumenVoxelLighting /
// the Lumen scene lit into a clipmap volume a global-SDF hit samples): one 4^3 workgroup per listed update brick of
// one clipmap, every cell of two voxels the visibility of the two directional lights (r = the sun, g = atmosphere
// light 1) from the field's surface nearest the cell's centre - the point a global-field hit there starts its shadow
// test from (Common/GIHitLighting.glslh GIHitRadianceGlobalField: the hit lifted along the field's normal by the far
// field's offset). The test is the far field's shadow ray (Common/GITraceDistanceField.glslh
// GITraceVisibilityGlobalField: no dither, any hit), cut where the ray leaves the field's occupied box (the same
// exact cut). The list is GlobalDistanceField::AddVoxelLightingPasses': the bricks the composite rewrote this frame
// and, after a change of the lights or of the scene, a window of the relight sweep.
// UE stores the lit radiance per voxel face; here the visibility alone - the hit's own guessed material, normal and
// bounce stay the hit's (GIHitRadianceGlobalField), so the picture is the per-hit march's, sampled.
Shader "GlobalDistanceFieldVoxelLighting"
{
    Compute
    {
        LocalSize(4, 4, 4);

        Uniform sampler LinearRepeatSampler;
        Uniform(0) texture3D GlobalDistanceField0Texture;
        #define GlobalDistanceField0Texture sampler3D(GlobalDistanceField0Texture, LinearRepeatSampler)
        Uniform(1) texture3D GlobalDistanceField1Texture;
        #define GlobalDistanceField1Texture sampler3D(GlobalDistanceField1Texture, LinearRepeatSampler)
        Uniform(2) texture3D GlobalDistanceField2Texture;
        #define GlobalDistanceField2Texture sampler3D(GlobalDistanceField2Texture, LinearRepeatSampler)
        Uniform(3) texture3D GlobalDistanceField3Texture;
        #define GlobalDistanceField3Texture sampler3D(GlobalDistanceField3Texture, LinearRepeatSampler)
        // Per listed brick: (brick x, y, z in world brick units of its clipmap, the clipmap).
        ReadBuffer(4) GDFVoxelLightingBricks
        {
            ivec4 VoxelLightingBricks[];
        };
        layout(binding = 5, rg8) restrict writeonly uniform image3D VoxelLightingTexture;

        PushConstant GDFVoxelLightingPush
        {
            vec4 Level0;      // the clipmaps' placement: centre.xyz cm, voxel cm (FrameField::Levels)
            vec4 Level1;
            vec4 Level2;
            vec4 Level3;
            vec4 OccupiedMin; // FrameField::OccupiedBox
            vec4 OccupiedMax;
            vec4 SunL;        // xyz = towards the sun (minus its travel), w = 1 when it is on and casts shadows
            vec4 Light1L;     // the same for atmosphere light 1
        };

        #include <Common/GlobalDistanceField.glslh>
        #include <Common/GlobalDistanceFieldVoxelLighting.glslh>

        #define GDF_VOXEL_LIGHTING_MAX_STEPS 128 // Common/GITraceDistanceField.glslh GI_GDF_MAX_STEPS

        // Common/GITraceDistanceField.glslh GIFarFieldOccupiedExit, over the push's box.
        float OccupiedExit(vec3 start, vec3 direction)
        {
            const vec3 boxMin = OccupiedMin.xyz;
            const vec3 boxMax = OccupiedMax.xyz;
            if (any(greaterThan(boxMin, boxMax)))
                return 0.0;
            const vec3  safe  = mix(direction, vec3(1.0e-8), lessThan(abs(direction), vec3(1.0e-8)));
            const vec3  inv   = 1.0 / safe;
            const vec3  t0    = (boxMin - start) * inv;
            const vec3  t1    = (boxMax - start) * inv;
            const vec3  lo    = min(t0, t1);
            const vec3  hi    = max(t0, t1);
            const float enter = max(max(lo.x, lo.y), max(lo.z, 0.0));
            const float leave = min(min(hi.x, hi.y), hi.z);
            return leave >= enter ? leave : 0.0;
        }

        // Common/GITraceDistanceField.glslh GITraceVisibilityGlobalField( origin, L, 0, 1e6 ).
        float Visibility(vec3 origin, vec3 L, vec4 levels[GDF_NUM_CLIPMAPS])
        {
            const float marchLength = min(1.0e6, OccupiedExit(origin, L));
            if (marchLength <= 0.0)
                return 1.0;
            return TraceGlobalDistanceField(origin, L, marchLength, GDF_VOXEL_LIGHTING_MAX_STEPS, levels, false, 0u,
                                            true).Hit ? 0.0 : 1.0;
        }

        // Common/GITraceDistanceField.glslh GITraceVoxelAt: the far field's offset (GIFarFieldOffset).
        float FarFieldOffset(vec3 position, vec4 levels[GDF_NUM_CLIPMAPS])
        {
            const int level = GlobalDistanceFieldLevelAt(position, levels);
            return level >= 0 ? levels[level].w : levels[GDF_NUM_CLIPMAPS - 1].w;
        }

        void main()
        {
            vec4 levels[GDF_NUM_CLIPMAPS];
            levels[0] = Level0;
            levels[1] = Level1;
            levels[2] = Level2;
            levels[3] = Level3;
            const ivec4 brick = VoxelLightingBricks[gl_WorkGroupID.x];
            const int   level = brick.w;
            const float voxel = levels[level].w;
            const float cellSize = GDFVoxelLightingCellSize(voxel);
            const ivec3 cell  = brick.xyz * GDF_VOXEL_LIGHTING_BRICK_CELLS + ivec3(gl_LocalInvocationID);
            const vec3  centre = (vec3(cell) + 0.5) * cellSize;

            // The surface a hit near the cell lies on: the cell's centre moved along the field's gradient onto the
            // zero set, then off it by the far field's offset, as GIHitRadianceGlobalField leaves a hit. A cell
            // farther than two cells from any surface is no hit's neighbour but a trilinear corner of one: its own
            // centre answers.
            vec3 origin = centre;
            const float d = SampleGlobalDistanceFieldLevel(level, centre, voxel);
            if (abs(d) < 2.0 * cellSize)
            {
                const vec3  gradient = GlobalDistanceFieldGradient(level, centre, voxel);
                const float length2  = dot(gradient, gradient);
                if (length2 > 1.0e-12)
                {
                    const vec3 N       = gradient * inversesqrt(length2);
                    const vec3 surface = centre - N * d;
                    origin             = surface + N * FarFieldOffset(surface, levels);
                }
            }
            const float sun    = SunL.w > 0.5 ? Visibility(origin, SunL.xyz, levels) : 1.0;
            const float light1 = Light1L.w > 0.5 ? Visibility(origin, Light1L.xyz, levels) : 1.0;
            imageStore(VoxelLightingTexture, GDFVoxelLightingTexel(level, cell), vec4(sun, light1, 0.0, 0.0));
        }
    }
}
