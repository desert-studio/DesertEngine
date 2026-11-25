// DesertAsset {"Kind":"Shader","Guid":"4af8c54e9ed9e5b29b192f5072886118","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterSurfFoam"
{
    Compute
    {
        // THE SURF FOAM HISTORY (WATER-RIPPLES-FFT; the whitecaps' history band tiles with the patch and knows no
        // depth, this one is in the world, over the ground: Water/WaterSurfFoam.glslh): foam is made where the FFT sea breaks on the
        // shallows and fades e-fold per the body material's FoamDecaySeconds, so a bore has a bright front and a
        // fading trail (the accumulate-and-decay foam buffer of AAA oceans).
        //
        // A cell is a water particle's still (X, Z), the label the foam rides (the orbits are closed, so a
        // particle's foam stays on its label: no advection). It makes foam while the passing wave is past the
        // breaker index (McCowan 1894: local height 2 eta over the still depth) AND its face is rising over the
        // particle; the crest, its back and the trough only carry what the front left.
        //
        // Dispatched after WaterFFTFinalize wrote this half (Graphic::System::WaterRenderer::AddWavePasses).
        #extension GL_EXT_samplerless_texture_functions : require
        #include <Water/WaterFFT.glslh>
        #include <Water/WaterSurfFoam.glslh>
        #include <Common/GerstnerWave.glslh>

        layout(binding = 0) uniform texture2D WaterInfoTexture;
        // Read (both halves' displacement, the previous half's foam) and written (this half's foam): disjoint texels.
        layout(binding = 2, rgba16f) restrict uniform image2D WaterFFTWaves;

        Buffer(3) WaterFFTParamsBuffer
        {
            WaterFFTParams Params;
        };

        LocalSize(8, 8, 1);

        // WaterGroundDepth (Mesh/Surface/WaterMeshTiles.glslh), the same filter over the same texels: the still
        // water column over the ground, 0 = no ground.
        float GroundDepth(vec2 worldXZ)
        {
            if (Params.SurfGround.w < 0.5)
                return 0.0;
            const vec2 uv = (worldXZ - Params.SurfGround.xy) * Params.SurfGround.z;
            if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
                return 0.0;
            const ivec2 size = ivec2(int(Params.SurfGround.w + 0.5));
            const vec2  p    = clamp(uv * vec2(size) - 0.5, vec2(0.0), vec2(size - 1));
            const ivec2 i0   = ivec2(floor(p));
            const ivec2 i1   = min(i0 + 1, size - 1);
            const vec2  f    = p - floor(p);
            const float h00  = texelFetch(WaterInfoTexture, i0, 0).r;
            const float h10  = texelFetch(WaterInfoTexture, ivec2(i1.x, i0.y), 0).r;
            const float h01  = texelFetch(WaterInfoTexture, ivec2(i0.x, i1.y), 0).r;
            const float h11  = texelFetch(WaterInfoTexture, i1, 0).r;
            if (min(min(h00, h10), min(h01, h11)) < -0.5e6)
                return 0.0;
            return max(Params.SurfWave.z - mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y), 1.0);
        }

        // One cascade's height at a plane point in the half that starts at band `base` (the wrapped bilinear of
        // WaterMeshTiles' WaterFFTSample).
        float CascadeHeight(int base, int cascade, vec2 plane)
        {
            const vec2  p   = plane / Params.Cascade[cascade] * float(WATER_FFT_SIZE) - 0.5;
            const ivec2 i0  = ivec2(floor(p)) & (WATER_FFT_SIZE - 1);
            const ivec2 i1  = (i0 + 1) & (WATER_FFT_SIZE - 1);
            const vec2  f   = p - floor(p);
            const int   row = (base + cascade) * WATER_FFT_SIZE;
            const float h00 = imageLoad(WaterFFTWaves, ivec2(i0.x, row + i0.y)).y;
            const float h10 = imageLoad(WaterFFTWaves, ivec2(i1.x, row + i0.y)).y;
            const float h01 = imageLoad(WaterFFTWaves, ivec2(i0.x, row + i1.y)).y;
            const float h11 = imageLoad(WaterFFTWaves, ivec2(i1.x, row + i1.y)).y;
            return mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
        }

        float SeaHeight(int base, vec2 plane, vec3 weight)
        {
            float height = 0.0;
            for (int c = 0; c < WATER_FFT_CASCADES; ++c)
            {
                if (weight[c] > 0.0)
                    height += weight[c] * CascadeHeight(base, c, plane);
            }
            return height;
        }

        // The foam this frame's wave makes on a cell, 0..1.
        float BreakingFront(vec2 world, float cell)
        {
            const float breaker = Params.Foam.z;
            const float dt      = Params.Foam.w;
            if (breaker <= 0.0 || dt <= 0.0 || Params.Foam.y < 0.5)
                return 0.0;
            const float depth = GroundDepth(world);
            if (depth <= 0.0)
                return 0.0;
            // The cascades the mesh draws at this distance (WaterMeshTiles' WaterFFTCascadeWeights), less the ones
            // the cell cannot carry (a patch of under 16 cells would be point-sampled noise).
            const float viewDist = distance(vec3(world.x, Params.SurfWave.z, world.y),
                                            vec3(Params.Surf.x, Params.Shape.w, Params.Surf.y));
            const vec3  patches  = viewDist / Params.Cascade.xyz;
            vec3        weight   = vec3(1.0, 1.0 - smoothstep(4.0, 8.0, patches.y), 1.0 - smoothstep(4.0, 8.0, patches.z));
            weight *= step(vec3(16.0 * cell), Params.Cascade.xyz);
            const float atten = GerstnerDepthAttenuation(depth, max(Params.SurfWave.w, 1.0));
            const vec2  plane = world - Params.SurfWave.xy;
            const float eta   = atten * SeaHeight(int(Params.Shape.z + 0.5), plane, weight);
            const float broke = smoothstep(0.7 * breaker, breaker, 2.0 * eta / depth);
            if (broke <= 0.0)
                return 0.0;
            // The face: rising by a tenth of the shallow-water wave speed is a full front.
            const float before = atten * SeaHeight(int(Params.Foam.y - 0.5), plane, weight);
            return broke * smoothstep(0.0, 0.1, (eta - before) / (dt * sqrt(WATER_FFT_GRAVITY * depth)));
        }

        void main()
        {
            const ivec2 id   = ivec2(gl_GlobalInvocationID.xy);
            vec3        kept = vec3(0.0);
            if (Params.Foam.y > 0.5)
            {
                const int row = (int(Params.Foam.y - 0.5) + WATER_FFT_SURF) * WATER_FFT_SIZE;
                kept = imageLoad(WaterFFTWaves, WaterSurfAtlasTexel(row, id)).xyz * Params.Foam.x;
            }
            vec3 foam = vec3(0.0);
            for (int level = 0; level < WATER_SURF_LEVELS; ++level)
            {
                // The world cell this texel holds now, and whether it held the same one in the previous window.
                const float cell   = WaterSurfCell(level);
                const ivec2 origin = WaterSurfOrigin(Params.Surf.xy, level);
                const ivec2 at     = origin + ((id - origin) & (WATER_SURF_SIZE - 1));
                const ivec2 was    = at - WaterSurfOrigin(Params.Surf.zw, level);
                const bool  held   = all(greaterThanEqual(was, ivec2(0))) && all(lessThan(was, ivec2(WATER_SURF_SIZE)));
                foam[level] = max(held ? kept[level] : 0.0, BreakingFront((vec2(at) + 0.5) * cell, cell));
            }
            const int row = (int(Params.Shape.z + 0.5) + WATER_FFT_SURF) * WATER_FFT_SIZE;
            imageStore(WaterFFTWaves, WaterSurfAtlasTexel(row, id), vec4(foam, 0.0));
        }
    }
}
