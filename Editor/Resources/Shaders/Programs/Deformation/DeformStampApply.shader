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
//   CLEAR (Window.w == 1: scroll, 2: whole level): the window moved with the view; every texel whose world cell
//   left the previous window now holds a new cell and is cleared to rest. Mode 2 clears all (a level's first use).
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
            ivec4 Previous; // xy: the previous window's minimum cell (mode 1); z: the batch's first tile (mode 0)
            vec4  Level;    // x: texel size, cm
        };

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
                    imageStore(DeformHeightTexture, texel, vec4(0.0));
                    imageStore(DeformDetailTexture, texel, vec4(0.5, 0.5, 0.0, 0.0));
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
