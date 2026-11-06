// DesertAsset {"Kind":"Shader","Guid":"c2f515b1cfce4d8faa20e1e3353fa3e3","Versions":{"SHDR":1},"Dependencies":[]}
// "SHADOWS: DISTANCE FIELD SUN", THE TRACE (UE DistanceFieldShadowing.usf DistanceFieldShadowingCS +
// RayTraceSingleShadowObject, written from memory): at HALF resolution, every receiver the band
// (Common/DistanceFieldShadow.glslh) gives to the distance fields cone-traces toward the sun - first through the
// distance fields of its light-space tile's list (each object's field sphere traced inside its world box; a
// landscape tile's field is its analytic heightfield, HeightfieldObjectDistance - the hills are in the lists like
// the meshes, UE's heightfield shadowing), then through the global distance field past
// DISTANCE_FIELD_SHADOW_OBJECT_RAY cm along the ray (the far occluders, what no list holds). The cone is the sun's
// disc: each step's distance over the cone's radius
// at that t is how much of the disc the surface leaves open, and the smallest of them over the ray is the visibility
// - a soft penumbra that widens with the occluder's distance (UE's MinConeVisibility). The receivers the cascades own
// whole, the sky and the receivers past the shadow distance write 1. r = visibility (1 = lit), g = the receiver's
// distance from the camera in metres (0 = sky) - what DistanceFieldShadowUpsample.shader weighs its taps by.
Shader "DistanceFieldShadowTrace"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/DistanceFieldShared.glslh>
        #include <Common/DistanceFieldShadow.glslh>
        #include <Common/ReconstructPosition.glslh>

        Uniform sampler PointClampSampler;
        Uniform sampler LinearRepeatSampler;

        ReadBuffer(0) DistanceFieldShadowParamsUB
        {
            DistanceFieldShadowParams ShadowParams;
        };
        ReadBuffer(1) DistanceFieldShadowTilesUB
        {
            uint TileData[];
        };
        // The mesh fields (DistanceField::MeshDistanceFieldReads; Common/MeshDistanceField.glslh's inputs).
        ReadBuffer(2) DFAssetData
        {
            vec4 DistanceFieldAssetData[];
        };
        ReadBuffer(3) DFIndirection
        {
            uint DistanceFieldIndirection[];
        };
        ReadBuffer(4) DFObjects
        {
            vec4 DistanceFieldObjects[];
        };
        Uniform(5) texture3D DistanceFieldBrickAtlasTexture;
        #define DistanceFieldBrickAtlasTexture sampler3D(DistanceFieldBrickAtlasTexture, LinearRepeatSampler)
        #include <Common/MeshDistanceField.glslh>

        // The global field's clipmaps (DistanceField::GlobalDistanceField::FrameField), read through the shared
        // repeat state: GlobalDistanceField.glslh wraps their toroidal windows.
        Uniform(6) texture3D GlobalDistanceField0Texture;
        #define GlobalDistanceField0Texture sampler3D(GlobalDistanceField0Texture, LinearRepeatSampler)
        Uniform(7) texture3D GlobalDistanceField1Texture;
        #define GlobalDistanceField1Texture sampler3D(GlobalDistanceField1Texture, LinearRepeatSampler)
        Uniform(8) texture3D GlobalDistanceField2Texture;
        #define GlobalDistanceField2Texture sampler3D(GlobalDistanceField2Texture, LinearRepeatSampler)
        Uniform(9) texture3D GlobalDistanceField3Texture;
        #define GlobalDistanceField3Texture sampler3D(GlobalDistanceField3Texture, LinearRepeatSampler)
        #include <Common/GlobalDistanceField.glslh>

        Uniform(10) texture2D GBufferDepthTexture;  // device depth, reversed-Z (0 = far: no surface)
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(11) texture2D GBufferNormalTexture; // rgb = world normal
        #define GBufferNormalTexture sampler2D(GBufferNormalTexture, PointClampSampler)
        layout(binding = 12, rgba16f) writeonly uniform image2D DistanceFieldShadowHalfTexture;

        // UE r.DFShadowRayStartOffsetDepthScale (0.003): the ray leaves its receiver by this share of the
        // receiver's distance from the camera - the fields' precision falls with the distance their voxels are
        // seen from - plus the normal bias.
        const float kRayStartOffsetDepthScale = 0.003;
        const float kNormalBias               = 4.0;   // cm, along the receiver's normal (DF_MESH_SDF_SURFACE_BIAS)
        // UE r.DFTwoSidedMeshDistanceBias (4 cm): a mostly two-sided mesh's sheets are thinner than its voxels;
        // its surface is grown by this much so a leaf card the field resolves to a V that never turns negative
        // still stops the cone.
        const float kTwoSidedMeshDistanceBias = 4.0;
        const int   kObjectSteps              = 32;    // sphere-trace steps per object (UE MaxSteps 32)
        const float kMinStep                  = 1.0;   // cm
        const float kMinSphere                = 1.0;   // cm: the cone's radius never below it
        // The mesh fields' share of the ray; the global field takes the rest out to DISTANCE_FIELD_SHADOW_RAY.
        #define DISTANCE_FIELD_SHADOW_OBJECT_RAY 10000.0
        #define DISTANCE_FIELD_SHADOW_RAY        50000.0
        const int kGlobalSteps = 64;

        // One object of the tile's list: the ray clipped to its world box, sphere traced through its field;
        // @p visibility lowered by every step's open share of the cone.
        float TraceObject(uint objectIndex, vec3 origin, vec3 direction, float tMin, float tanHalf, float visibility)
        {
            const uint record = objectIndex * uint(DF_OBJECT_STRIDE);
            const vec3 boxMin = DistanceFieldObjects[record + uint(DF_OBJECT_ROW_BOUNDS_MIN)].xyz;
            const vec3 boxMax = DistanceFieldObjects[record + uint(DF_OBJECT_ROW_BOUNDS_MAX)].xyz;
            const vec3 inv    = 1.0 / mix(vec3(1.0e-6), direction, greaterThan(abs(direction), vec3(1.0e-6)));
            const vec3 t0     = (boxMin - origin) * inv;
            const vec3 t1     = (boxMax - origin) * inv;
            const vec3 tLo    = min(t0, t1);
            const vec3 tHi    = max(t0, t1);
            const float enter = max(max(tLo.x, tLo.y), max(tLo.z, tMin));
            const float leave = min(min(tHi.x, tHi.y), min(tHi.z, DISTANCE_FIELD_SHADOW_OBJECT_RAY));
            if (leave < enter)
                return visibility;
            const uint  flags = floatBitsToUint(DistanceFieldObjects[record + uint(DF_OBJECT_ROW_FLAGS)].x);
            const float bias  = (flags & uint(DF_OBJECT_FLAG_MOSTLY_TWO_SIDED)) != 0u ? kTwoSidedMeshDistanceBias : 0.0;
            float t = enter;
            for (int s = 0; s < kObjectSteps && t <= leave; ++s)
            {
                const float distance = DistanceToNearestSurfaceForObject(objectIndex, origin + direction * t, 1.0e6) - bias;
                const float sphere   = max(t * tanHalf, kMinSphere);
                visibility = min(visibility, clamp(distance / sphere, 0.0, 1.0));
                if (visibility <= 0.0)
                    return 0.0;
                t += max(distance, kMinStep);
            }
            return visibility;
        }

        // The global field from @p tStart: the far occluders, through the finest clipmap holding each step.
        float TraceGlobalField(vec3 origin, vec3 direction, float tStart, float tanHalf, float visibility)
        {
            if (ShadowParams.gdfLevels[0].w <= 0.0)
                return visibility;
            float t = tStart;
            for (int s = 0; s < kGlobalSteps && t < DISTANCE_FIELD_SHADOW_RAY; ++s)
            {
                const vec3 p     = origin + direction * t;
                const int  level = GlobalDistanceFieldLevelAt(p, ShadowParams.gdfLevels);
                if (level < 0)
                    break;
                const float voxel    = ShadowParams.gdfLevels[level].w;
                const float distance = SampleGlobalDistanceFieldLevel(level, p, voxel);
                // Below half a voxel the trilinear field cannot tell the cone's radius apart.
                const float sphere = max(t * tanHalf, 0.5 * voxel);
                visibility = min(visibility, clamp(distance / sphere, 0.0, 1.0));
                if (visibility <= 0.0)
                    return 0.0;
                t += max(distance, 0.5 * voxel);
            }
            return visibility;
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            const ivec2 size  = ivec2(ShadowParams.extent.zw);
            if (pixel.x >= size.x || pixel.y >= size.y)
                return;
            // The half-resolution texel's top-left full-resolution pixel: the receiver it stands for.
            const ivec2 full  = min(pixel * 2, ivec2(ShadowParams.extent.xy) - 1);
            const float depth = texelFetch(GBufferDepthTexture, full, 0).r;
            if (depth <= 0.0)
            {
                imageStore(DistanceFieldShadowHalfTexture, pixel, vec4(1.0, 0.0, 0.0, 0.0));
                return;
            }
            const vec2  uv       = (vec2(full) + 0.5) / ShadowParams.extent.xy;
            const vec3  position = ReconstructWorldPosition(uv, depth, ShadowParams.invViewProjection);
            const float distance = length(position - ShadowParams.camera.xyz);
            const vec4  lit      = vec4(1.0, distance * 0.01, 0.0, 0.0);
            if (distance > ShadowParams.receivers.z)
            {
                imageStore(DistanceFieldShadowHalfTexture, pixel, lit);
                return;
            }
            const float band = ShadowParams.camera.w > 0.5
                                    ? DistanceFieldShadowBandWeight(ShadowParams.lastCascade, position, ShadowParams.receivers.w)
                                    : 1.0;
            if (band <= 0.0)
            {
                imageStore(DistanceFieldShadowHalfTexture, pixel, lit);
                return;
            }

            const vec3  toSun   = ShadowParams.lightZ.xyz;
            const vec3  normal  = normalize(texelFetch(GBufferNormalTexture, full, 0).rgb);
            const vec3  origin  = position + normal * (dot(normal, toSun) >= 0.0 ? kNormalBias : -kNormalBias);
            const float tMin    = kRayStartOffsetDepthScale * distance;
            const float tanHalf = ShadowParams.receivers.y;

            float visibility = 1.0;
            const int tile = DistanceFieldShadowTileAt(ShadowParams, DistanceFieldShadowLight(ShadowParams, origin).xy);
            if (tile >= 0)
            {
                const uint header = uint(tile) * DistanceFieldShadowHeaderStride;
                const uint first  = TileData[header];
                const uint length = TileData[header + 1u];
                for (uint k = 0u; k < length && visibility > 0.0; ++k)
                    visibility = TraceObject(TileData[first + k], origin, toSun, tMin, tanHalf, visibility);
            }
            if (visibility > 0.0)
                visibility = TraceGlobalField(origin, toSun, max(DISTANCE_FIELD_SHADOW_OBJECT_RAY, tMin), tanHalf, visibility);
            // The cone's open share to a disc's visible area (UE: the smoothstep of MinConeVisibility).
            visibility = smoothstep(0.0, 1.0, visibility);
            imageStore(DistanceFieldShadowHalfTexture, pixel, vec4(visibility, distance * 0.01, 0.0, 0.0));
        }
    }
}
