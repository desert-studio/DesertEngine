// DesertAsset {"Kind":"Shader","Guid":"78668f422e004da4abeb15c228703c41","Versions":{"SHDR":1},"Dependencies":[]}
// THE DEFORMATION FIELD'S ONE PROGRAM (DEFORM-FIELD; Graphic/Deformation/DeformationFieldRenderer). Two modes:
//
//   APPLY (Window.w == 0): one dispatch per batch of contacts that name the same stamp, per clipmap level. The CPU
//   binned the batch's contacts into 64x64-texel tiles of the level (Tiles: tile x, y in world tiles, first entry
//   in TileStamps, count); a workgroup z is one tile, its 8x8 groups of 8x8 threads its 4096 texels. ONE THREAD
//   OWNS ONE TEXEL and loops its tile's contacts in submission order - read, combine, write - so the result is the
//   order the world submitted them in and no atomics are needed (RoTR/HZD-style deformation, UE RVT-style binning;
//   the Windows lead's choice (b), DEFORM-FIELD p1). A texel is in exactly one tile, so no two threads of a
//   dispatch touch it; consecutive dispatches are ordered by the render graph (StorageWrite after StorageWrite).
//
//   SCROLL (Window.w == 1: scroll, 2: whole level): the window moved with the view; every texel whose world cell
//   left the previous window now holds a new cell. Mode 2 renews all of them (a level's first use). A renewed
//   texel is REFILLED FROM THE COARSER LEVEL (Level.y > 0: level 0 from level 1, bilinear between its four cells)
//   - the clipmap's coarser level is the finer one's backing store, so a print the view walked away from (out of
//   level 0, still in level 1) comes back when the view returns instead of being lost to the scroll (DEFORM-GRASS-4:
//   DEFORM-FIELD.scn walks 30 m out and back, and 4_grass_lit read a cleared level 0). The coarsest level has
//   nothing behind it and renews to rest; the field beyond it is DEFORM-PERSIST's.
//
// Addressing is toroidal: a world cell c (floor(xz / texel)) lives at texel c mod N, N a power of two, so a moved
// window rewrites nothing that stayed inside it.
Shader "DeformStampApply"
{
    Compute
    {
        // Formats declared: a read-modify-write needs none of shaderStorageImageReadWithoutFormat.
        layout(binding = 0, rgba16f) restrict uniform image2D DeformHeightTexture; // sink, rim, compaction, flatten
        layout(binding = 1, rgba8) restrict uniform image2D DeformDetailTexture;   // flatten dir xz, wetness, touched
        // The next coarser level (the refill's source; read only in modes 1 and 2 with Level.y > 0). Every pass binds
        // the other level here, so the layout is the same for every mode and stamp variant.
        layout(binding = 5, rgba16f) restrict readonly uniform image2D DeformCoarseHeightTexture;
        layout(binding = 6, rgba8) restrict readonly uniform image2D DeformCoarseDetailTexture;
        // Per contact, 4 x vec4: (x, y, z, radius), (pressure, speed, heading, mass), (slip x, slip z, age, seed),
        // (normal xyz, 0).
        ReadBuffer(2) DeformStamps
        {
            vec4 Stamps[];
        };
        // Per tile of this upload: (tile x, tile y, first TileStamps entry, count).
        ReadBuffer(3) DeformTiles
        {
            ivec4 Tiles[];
        };
        ReadBuffer(4) DeformTileStamps
        {
            uint TileStamps[];
        };

        PushConstant DeformApplyPush
        {
            ivec4 Window;   // xy: the window's minimum cell, z: resolution N, w: mode (0 apply, 1 scroll, 2 clear all)
            ivec4 Previous; // xy: the previous window's minimum cell (mode 1); z: the batch's first tile (mode 0);
                            // zw: the coarser level's minimum cell (modes 1, 2)
            vec4  Level;    // x: texel size, cm; y: the coarser level's texel (0: none, renew to rest), z: its N
        };

        // The rest value of a texel: untouched ground.
        const vec4 kDeformHeightRest = vec4(0.0);
        const vec4 kDeformDetailRest = vec4(0.5, 0.5, 0.0, 0.0);

        // The coarser level's texel holding coarse cell @p c, or rest outside its window.
        void DeformCoarseFetch(ivec2 c, out vec4 h, out vec4 d)
        {
            const int n = int(Level.z);
            if (any(lessThan(c, Previous.zw)) || any(greaterThanEqual(c, Previous.zw + ivec2(n))))
            {
                h = kDeformHeightRest;
                d = kDeformDetailRest;
                return;
            }
            const ivec2 t = c & ivec2(n - 1);
            h = imageLoad(DeformCoarseHeightTexture, t);
            d = imageLoad(DeformCoarseDetailTexture, t);
        }

        // Fine cell @p cell's renewed value from the coarser level: its four cells around the fine cell's centre,
        // bilinear. Height, wetness: weighted. Flatten direction: the flatten-weighted sum of the four unit
        // directions, renormalised (a print's centre, where they cancel, keeps the strongest one's). Touched: max.
        void DeformRefill(ivec2 cell, out vec4 h, out vec4 d)
        {
            const vec2  u  = (vec2(cell) + 0.5) * Level.x / Level.y - 0.5;
            const ivec2 c0 = ivec2(floor(u));
            const vec2  f  = u - vec2(c0);
            float w[4] = float[4]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
            ivec2 o[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));
            h = vec4(0.0);
            float wet     = 0.0;
            float touched = 0.0;
            vec2  dir     = vec2(0.0);
            vec2  strong  = vec2(0.0);
            float best    = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                vec4 ch;
                vec4 cd;
                DeformCoarseFetch(c0 + o[k], ch, cd);
                h += ch * w[k];
                wet += cd.z * w[k];
                touched = max(touched, cd.w);
                const vec2 along = cd.xy * 2.0 - 1.0;
                dir += along * (ch.a * w[k]);
                if (ch.a > best)
                {
                    best   = ch.a;
                    strong = along;
                }
            }
            const vec2 laid = dot(dir, dir) > 1e-8 ? normalize(dir) : strong;
            d = vec4(laid * 0.5 + 0.5, wet, touched);
            if (best <= 0.0)
                d.xy = kDeformDetailRest.xy;
        }

        #include <Deformation/DeformStampContract.glslh>
        #include <Deformation/DeformStampBody.glslh>

        LocalSize(8, 8, 1);
        void main()
        {
            const int N = Window.z;
            if (Window.w != 0)
            {
                const ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
                if (texel.x >= N || texel.y >= N)
                    return;
                // The cell this texel holds in the new window.
                const ivec2 cell = Window.xy + ((texel - Window.xy) & ivec2(N - 1));
                const bool  kept = Window.w == 1 && all(greaterThanEqual(cell, Previous.xy)) &&
                                   all(lessThan(cell, Previous.xy + ivec2(N)));
                if (!kept)
                {
                    vec4 h = kDeformHeightRest;
                    vec4 d = kDeformDetailRest;
                    if (Level.y > 0.0)
                        DeformRefill(cell, h, d);
                    imageStore(DeformHeightTexture, texel, h);
                    imageStore(DeformDetailTexture, texel, d);
                }
                return;
            }

            const ivec4 tile = Tiles[Previous.z + int(gl_WorkGroupID.z)];
            const ivec2 cell = tile.xy * 64 + ivec2(gl_GlobalInvocationID.xy);
            if (any(lessThan(cell, Window.xy)) || any(greaterThanEqual(cell, Window.xy + ivec2(N))))
                return;
            const ivec2 texel = cell & ivec2(N - 1);
            vec4        h     = imageLoad(DeformHeightTexture, texel);
            vec4        d     = imageLoad(DeformDetailTexture, texel);

            DeformStampInputs i;
            i.WorldXZ   = (vec2(cell) + 0.5) * Level.x;
            i.TexelSize = Level.x;
            for (int k = 0; k < tile.w; ++k)
            {
                const uint s  = TileStamps[tile.z + k];
                const vec4 a0 = Stamps[s * 4u + 0u];
                const vec4 a1 = Stamps[s * 4u + 1u];
                const vec4 a2 = Stamps[s * 4u + 2u];
                const vec4 a3 = Stamps[s * 4u + 3u];
                const vec2 rel = i.WorldXZ - a0.xz;
                const float c  = cos(a1.z);
                const float sn = sin(a1.z);
                i.Local         = vec2(dot(rel, vec2(c, -sn)), dot(rel, vec2(sn, c))) / a0.w;
                if (any(greaterThan(abs(i.Local), vec2(DEFORM_STAMP_REACH))))
                    continue;
                i.Radius        = a0.w;
                i.Pressure      = a1.x;
                i.Speed         = a1.y;
                i.Heading       = a1.z;
                i.Mass          = a1.w;
                i.SlipVector    = a2.xy;
                i.Age           = a2.z;
                i.Seed          = a2.w;
                i.ContactNormal = a3.xyz;

                DeformStampOutputs o = DeformStampOutputsNone();
                DeformStampEvaluate(i, o);

                const float sink = min(o.Sink, 0.0);
                if (sink < h.r)
                {
                    h.r = sink;
                    h.g = 0.0; // pressed deeper than before: the rim that stood here is under the foot
                }
                h.g = min(h.g + max(o.Rim, 0.0), DEFORM_RIM_MAX_CM);
                h.b = max(h.b, clamp(o.Compaction, 0.0, 1.0));
                const float flatten = clamp(o.Flatten, 0.0, 1.0);
                if (flatten > h.a)
                {
                    h.a  = flatten;
                    d.xy = normalize(o.FlattenDirection + vec2(1e-6, 0.0)) * 0.5 + 0.5;
                }
                d.z = max(d.z, clamp(o.Wetness, 0.0, 1.0));
                d.w = 1.0;
            }
            imageStore(DeformHeightTexture, texel, h);
            imageStore(DeformDetailTexture, texel, d);
        }
    }
}
