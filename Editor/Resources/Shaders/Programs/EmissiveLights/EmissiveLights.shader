// DesertAsset {"Kind":"Shader","Guid":"b7e41c2d95a84f6e8a0d3c51f29e7a64","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCENE'S EMISSIVE TRIANGLES (Common/EmissiveLights.glslh; RayTracing::EmissiveLightsRenderer): "Build" expands
// the emissive segments of the list into world triangles and their powers, "Reduce" sums the power tree level by
// level - once per part of the list: the static part when its hash changes (kept across frames otherwise), the
// dynamic part (the skinned primitives' segments, read from the skin cache's positions) every frame. NVIDIA RTXDI
// PrepareLights is the model (one thread per emissive triangle, the positions its BLAS was built from read by
// address, world transform applied, its
// power from the material's emission), adapted: the power tree is a buffer of levels (as the path tracer's sky
// pdf, PathTracerSky.shader) instead of a PDF texture's mips.
Shader "EmissiveLights"
{
    Pass "Build"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #extension GL_EXT_buffer_reference : require
            #extension GL_EXT_buffer_reference_uvec2 : require

            #include <Common/EmissiveLights.glslh>

            layout(std430, binding = 0) readonly buffer EmissiveSegmentsUB
            {
                EmissiveSegment emissiveSegments[];
            };
            layout(std430, binding = 1) writeonly buffer EmissiveTrianglesUB
            {
                EmissiveTriangle emissiveTriangles[];
            };
            layout(std430, binding = 2) writeonly buffer EmissivePowerUB
            {
                float emissivePower[];
            };

            PushConstant PushConstants
            {
                uvec4 u_List; // x = triangles, y = leaves (a power of two >= x), z = segments
            };

            layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer EmissiveUintRef
            {
                uint Value;
            };

            uvec2 EmissiveAddressAdd(uvec2 address, uint bytes)
            {
                uint       carry;
                const uint lo = uaddCarry(address.x, bytes, carry);
                return uvec2(lo, address.y + carry);
            }

            vec3 EmissivePosition(EmissiveSegment s, uint corner)
            {
                const uint  index = EmissiveUintRef(EmissiveAddressAdd(s.IndexAddress, (s.FirstIndex + corner) * 4u)).Value;
                const uvec2 base  = EmissiveAddressAdd(s.VertexAddress, (index + s.FirstVertex) * s.VertexStride + s.PositionOffset);
                const vec3  p     = vec3(uintBitsToFloat(EmissiveUintRef(base).Value),
                                         uintBitsToFloat(EmissiveUintRef(EmissiveAddressAdd(base, 4u)).Value),
                                         uintBitsToFloat(EmissiveUintRef(EmissiveAddressAdd(base, 8u)).Value));
                return vec3(dot(s.ObjectToWorld[0], vec4(p, 1.0)), dot(s.ObjectToWorld[1], vec4(p, 1.0)),
                            dot(s.ObjectToWorld[2], vec4(p, 1.0)));
            }

            // The segment whose triangle range holds @p triangle: the rows are end to end in ascending FirstTriangle.
            uint EmissiveSegmentOf(uint triangle)
            {
                uint lo = 0u;
                uint hi = u_List.z - 1u;
                while (lo < hi)
                {
                    const uint mid = (lo + hi + 1u) / 2u;
                    if (emissiveSegments[mid].FirstTriangle <= triangle)
                        lo = mid;
                    else
                        hi = mid - 1u;
                }
                return lo;
            }

            // One leaf of the tree per thread; the leaves past the list are zero (never picked).
            void main()
            {
                const uint leaf = gl_GlobalInvocationID.x;
                if (leaf >= u_List.y)
                    return;
                if (leaf >= u_List.x)
                {
                    emissivePower[leaf] = 0.0;
                    return;
                }
                const uint            row       = EmissiveSegmentOf(leaf);
                const EmissiveSegment s         = emissiveSegments[row];
                const uint            firstCorner = (leaf - s.FirstTriangle) * 3u;
                const vec3            p0        = EmissivePosition(s, firstCorner);
                const vec3            e1        = EmissivePosition(s, firstCorner + 1u) - p0;
                const vec3            e2        = EmissivePosition(s, firstCorner + 2u) - p0;
                const float           area      = 0.5 * length(cross(e1, e2));
                const float           power     = s.Radiance.w * area;
                EmissiveTriangle      t;
                t.P0Area                  = vec4(p0, area);
                t.Edge1Power              = vec4(e1, power);
                t.Edge2Segment            = vec4(e2, uintBitsToFloat(row));
                emissiveTriangles[leaf]   = t;
                emissivePower[leaf]       = power;
            }
        }
    }

    Pass "Reduce"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #include <Common/EmissiveLights.glslh>

            layout(std430, binding = 0) buffer EmissivePowerUB
            {
                float emissivePower[];
            };

            PushConstant PushConstants
            {
                uvec4 u_List; // y = leaves, w = the level this dispatch writes (>= 1)
            };

            // One node of level w: the SUM of its two children in level w - 1, so the root is the total power.
            void main()
            {
                const uint leaves = u_List.y;
                const uint level  = u_List.w;
                const uint node   = gl_GlobalInvocationID.x;
                if (node >= (leaves >> level))
                    return;
                const uint below = EmissivePowerLevelOffset(leaves, level - 1u) + 2u * node;
                emissivePower[EmissivePowerLevelOffset(leaves, level) + node] = emissivePower[below] + emissivePower[below + 1u];
            }
        }
    }
}
