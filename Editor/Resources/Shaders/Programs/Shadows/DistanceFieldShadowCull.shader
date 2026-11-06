// DesertAsset {"Kind":"Shader","Guid":"f2ae27cbd3634e2da0904a1d55a5ece0","Versions":{"SHDR":1},"Dependencies":[]}
// "SHADOWS: DISTANCE FIELD SUN", THE TILE CULL (UE CullObjectsForShadowCS + the light tile culling of
// DistanceFieldShadowing.cpp, written from memory; a compute walk in place of UE's rasterised object bounds): one
// thread per light-space tile (Common/DistanceFieldShadow.glslh) walks every object - the meshes, then the
// landscape tiles (DF_OBJECT_FLAG_HEIGHTFIELD, MeshDistanceFieldReads::NumObjects: UE culls heightfields into the
// same lists) - 64 at a time through shared memory, and keeps each one whose light-space footprint - grown by the
// penumbra its top casts onto the lowest
// receiver - overlaps the tile and whose top stands above the lowest receiver (an object wholly below every receiver
// lies on the far side of each from the sun). Dispatched twice like the near field's grid (MeshSDFCullGrid.shader):
// CullPass 0 COUNTS into the tile's header, DistanceFieldShadowCullOffsets.shader sums the counts into each list's
// first index, CullPass 1 SCATTERS the same objects, in the object order, into the list. No per-tile cap: a frame
// whose lists need more than the room is refused whole by the offsets pass.
Shader "DistanceFieldShadowCull"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/DistanceFieldShared.glslh>
        #include <Common/DistanceFieldShadow.glslh>

        ReadBuffer(0) DistanceFieldShadowParamsUB
        {
            DistanceFieldShadowParams ShadowParams;
        };
        ReadBuffer(1) DFObjects
        {
            vec4 DistanceFieldObjects[];
        };
        // The tiles' headers (DistanceFieldShadowHeaderStride uints each: the list's first index, its length), then
        // the lists.
        Buffer(2) DistanceFieldShadowTilesUB
        {
            uint TileData[];
        };
        PushConstant DistanceFieldShadowCullPush
        {
            uint CullPass; // 0 = count into the headers, 1 = scatter into the lists
        };

        const uint kBatch = 64u;

        shared vec4 s_Footprint[kBatch]; // light-space x min, y min, x max, y max (cm), grown by the penumbra
        shared uint s_Object[kBatch];    // the object index; 0xFFFFFFFF = below every receiver, culled

        void main()
        {
            const uint  tile  = gl_GlobalInvocationID.x;
            const bool  valid = tile < ShadowParams.counts.z;
            const vec2  cell  = vec2(float(tile % ShadowParams.counts.y), float(tile / ShadowParams.counts.y));
            const vec2  size  = 1.0 / ShadowParams.tiles.zw;
            const vec2  lo    = ShadowParams.tiles.xy + cell * size;
            const vec2  hi    = lo + size;
            const uint  header  = tile * DistanceFieldShadowHeaderStride;
            const bool  scatter = CullPass != 0u;
            const uint  first   = scatter && valid ? TileData[header] : 0u;
            const uint  length  = scatter && valid ? TileData[header + 1u] : 0u;

            uint       n     = 0u;
            const uint total = ShadowParams.counts.x;
            for (uint base = 0u; base < total; base += kBatch)
            {
                const uint i = base + gl_LocalInvocationIndex;
                if (i < total)
                {
                    const uint record = i * uint(DF_OBJECT_STRIDE);
                    const vec3 boxMin = DistanceFieldObjects[record + uint(DF_OBJECT_ROW_BOUNDS_MIN)].xyz;
                    const vec3 boxMax = DistanceFieldObjects[record + uint(DF_OBJECT_ROW_BOUNDS_MAX)].xyz;
                    const vec3 centre = DistanceFieldShadowLight(ShadowParams, 0.5 * (boxMin + boxMax));
                    const vec3 radius = 0.5 * (boxMax - boxMin);
                    // The world box's half extent along each light axis.
                    const vec3 reach  = vec3(dot(abs(ShadowParams.lightX.xyz), radius),
                                             dot(abs(ShadowParams.lightY.xyz), radius),
                                             dot(abs(ShadowParams.lightZ.xyz), radius));
                    const float top    = centre.z + reach.z;
                    const float above  = top - ShadowParams.receivers.x;
                    // A receiver's cone toward the sun widens by tan(angle / 2) per cm: an object that high above
                    // the lowest receiver shades a receiver up to that much beside its footprint.
                    const float grow   = max(above, 0.0) * ShadowParams.receivers.y;
                    s_Footprint[gl_LocalInvocationIndex] = vec4(centre.xy - reach.xy - grow, centre.xy + reach.xy + grow);
                    s_Object[gl_LocalInvocationIndex]    = above >= 0.0 ? i : 0xFFFFFFFFu;
                }
                barrier();
                if (valid && (!scatter || n < length))
                {
                    const uint count = min(kBatch, total - base);
                    for (uint k = 0u; k < count; ++k)
                    {
                        if (s_Object[k] == 0xFFFFFFFFu)
                            continue;
                        const vec4 f = s_Footprint[k];
                        if (f.x > hi.x || f.z < lo.x || f.y > hi.y || f.w < lo.y)
                            continue;
                        if (scatter)
                        {
                            if (n == length)
                                break;
                            TileData[first + n] = s_Object[k];
                        }
                        ++n;
                    }
                }
                barrier();
            }
            if (valid && !scatter)
            {
                TileData[header]      = 0u;
                TileData[header + 1u] = n;
            }
        }
    }
}
