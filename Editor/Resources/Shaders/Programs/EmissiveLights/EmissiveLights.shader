// DesertAsset {"Kind":"Shader","Guid":"b7e41c2d95a84f6e8a0d3c51f29e7a64","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCENE'S EMISSIVE TRIANGLES (Common/EmissiveLights.glslh; RayTracing::EmissiveLightsRenderer): "Build" expands
// the emissive segments of the list into world triangles and their powers, "Reduce" sums the power tree level by
// level, "Move" writes a moved row's transform - per part of the list: the static part whole when its structure
// changes (an emitter added, removed or re-materialled), and only the moved rows' leaves and their path to the root
// when only transforms moved (kept across frames otherwise); the dynamic part (the skinned primitives' segments,
// read from the skin cache's positions) whole every frame. Build and Reduce write the RUNS of nodes the CPU names
// (EmissiveNodeRun, RayTracing::EmissiveDirtyRuns): the whole tree is one run per level.
// NVIDIA RTXDI PrepareLights is the model (one thread per emissive triangle, the positions its BLAS was built from
// read by address, world transform applied, its power from the material's emission and the emissive texture's
// average over the triangle - RTXDI reads the texture's mip whose texel covers the triangle's UV footprint),
// adapted: the power tree is a buffer of levels (as the path tracer's sky pdf, PathTracerSky.shader) instead of a
// PDF texture's mips.
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
            #include <Common/BindlessTextures.glslh>
            #include <Common/SurfaceInputs.glslh>
            #include <Mesh/MeshVertexAccess.glslh>

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
            layout(std430, binding = 3) readonly buffer EmissiveRunsUB
            {
                EmissiveNodeRun emissiveRuns[];
            };

            PushConstant PushConstants
            {
                uvec4 ListCounts; // x = triangles, y = leaves (a power of two >= x), z = segments
                uvec4 Runs;       // x = first run of level 0, y = its run count, z = their leaf total
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

            // The vertex of corner @p corner of the segment's index run (FirstVertex applied).
            uint EmissiveVertex(EmissiveSegment s, uint corner)
            {
                return EmissiveUintRef(EmissiveAddressAdd(s.IndexAddress, (s.FirstIndex + corner) * 4u)).Value + s.FirstVertex;
            }

            vec3 EmissivePosition(EmissiveSegment s, uint vertex)
            {
                const uvec2 base = EmissiveAddressAdd(s.VertexAddress, vertex * s.VertexStride + s.PositionOffset);
                const vec3  p    = vec3(uintBitsToFloat(EmissiveUintRef(base).Value),
                                        uintBitsToFloat(EmissiveUintRef(EmissiveAddressAdd(base, 4u)).Value),
                                        uintBitsToFloat(EmissiveUintRef(EmissiveAddressAdd(base, 8u)).Value));
                return vec3(dot(s.ObjectToWorld[0], vec4(p, 1.0)), dot(s.ObjectToWorld[1], vec4(p, 1.0)),
                            dot(s.ObjectToWorld[2], vec4(p, 1.0)));
            }

            // The UV the surface template samples the emissive map at (StandardSurface: the stored UV in the raster's
            // convention, then KHR_texture_transform).
            vec2 EmissiveUV(EmissiveSegment s, uint vertex)
            {
                const uvec2 base   = EmissiveAddressAdd(s.AttributeAddress, vertex * s.AttributeStride + s.UvOffset);
                const vec2  stored = vec2(uintBitsToFloat(EmissiveUintRef(base).Value),
                                          uintBitsToFloat(EmissiveUintRef(EmissiveAddressAdd(base, 4u)).Value));
                return SurfaceTransformUV(DesertMeshTextureUV(stored), s.UvTransform.xy, s.UvTransform.zw, s.UvRotation);
            }

            // The emissive map's average over the triangle's UV footprint: the mip whose one texel covers the
            // footprint's area, read at the UV centroid (its bilinear 2x2 of that mip spans the footprint). No map: 1.
            vec3 EmissiveTexelAverage(EmissiveSegment s, uint v0, uint v1, uint v2)
            {
                if (s.EmissiveTexture == kEmissiveNoTexture)
                    return vec3(1.0);
                const BindlessTexture map    = BindlessTexture(s.EmissiveTexture, s.EmissiveSampler, 0u, 0u);
                const vec2            uv0    = EmissiveUV(s, v0);
                const vec2            uv1    = EmissiveUV(s, v1);
                const vec2            uv2    = EmissiveUV(s, v2);
                const vec2            du     = uv1 - uv0;
                const vec2            dv     = uv2 - uv0;
                const vec2            size   = vec2(BindlessTextureSize(map, 0));
                const float           texels = 0.5 * abs(du.x * dv.y - du.y * dv.x) * size.x * size.y;
                const float           lod    = 0.5 * log2(max(texels, 1.0));
                return max(SampleBindlessLod(map, (uv0 + uv1 + uv2) / 3.0, lod).rgb, vec3(0.0));
            }

            // The segment whose triangle range holds @p triangle: the rows are end to end in ascending FirstTriangle.
            uint EmissiveSegmentOf(uint triangle)
            {
                uint lo = 0u;
                uint hi = ListCounts.z - 1u;
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

            // The leaf thread @p thread writes: in the run whose ThreadPrefix is the last at or below it.
            uint EmissiveRunNode(uint thread)
            {
                uint lo = Runs.x;
                uint hi = Runs.x + Runs.y - 1u;
                while (lo < hi)
                {
                    const uint mid = (lo + hi + 1u) / 2u;
                    if (emissiveRuns[mid].ThreadPrefix <= thread)
                        lo = mid;
                    else
                        hi = mid - 1u;
                }
                return emissiveRuns[lo].First + (thread - emissiveRuns[lo].ThreadPrefix);
            }

            // One leaf of the tree per thread; the leaves past the list are zero (never picked).
            void main()
            {
                const uint thread = gl_GlobalInvocationID.x;
                if (thread >= Runs.z)
                    return;
                const uint leaf = EmissiveRunNode(thread);
                if (leaf >= ListCounts.y)
                    return;
                if (leaf >= ListCounts.x)
                {
                    emissivePower[leaf] = 0.0;
                    return;
                }
                const uint            row         = EmissiveSegmentOf(leaf);
                const EmissiveSegment s           = emissiveSegments[row];
                const uint            firstCorner = (leaf - s.FirstTriangle) * 3u;
                const uint            v0          = EmissiveVertex(s, firstCorner);
                const uint            v1          = EmissiveVertex(s, firstCorner + 1u);
                const uint            v2          = EmissiveVertex(s, firstCorner + 2u);
                const vec3            p0          = EmissivePosition(s, v0);
                const vec3            e1          = EmissivePosition(s, v1) - p0;
                const vec3            e2          = EmissivePosition(s, v2) - p0;
                const float           area        = 0.5 * length(cross(e1, e2));
                const vec3            emitted     = s.Radiance.rgb * EmissiveTexelAverage(s, v0, v1, v2);
                const float           power       = dot(emitted, vec3(0.2126, 0.7152, 0.0722)) * area;
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
            layout(std430, binding = 1) readonly buffer EmissiveRunsUB
            {
                EmissiveNodeRun emissiveRuns[];
            };

            PushConstant PushConstants
            {
                uvec4 ListCounts; // y = leaves, w = the level this dispatch writes (>= 1)
                uvec4 Runs;       // x = first run of that level, y = its run count, z = their node total
            };

            // The node thread @p thread writes: in the run whose ThreadPrefix is the last at or below it.
            uint EmissiveRunNode(uint thread)
            {
                uint lo = Runs.x;
                uint hi = Runs.x + Runs.y - 1u;
                while (lo < hi)
                {
                    const uint mid = (lo + hi + 1u) / 2u;
                    if (emissiveRuns[mid].ThreadPrefix <= thread)
                        lo = mid;
                    else
                        hi = mid - 1u;
                }
                return emissiveRuns[lo].First + (thread - emissiveRuns[lo].ThreadPrefix);
            }

            // One node of level w: the SUM of its two children in level w - 1, so the root is the total power.
            void main()
            {
                const uint leaves = ListCounts.y;
                const uint level  = ListCounts.w;
                const uint thread = gl_GlobalInvocationID.x;
                if (thread >= Runs.z)
                    return;
                const uint node = EmissiveRunNode(thread);
                if (node >= (leaves >> level))
                    return;
                const uint below = EmissivePowerLevelOffset(leaves, level - 1u) + 2u * node;
                emissivePower[EmissivePowerLevelOffset(leaves, level) + node] = emissivePower[below] + emissivePower[below + 1u];
            }
        }
    }

    Pass "Move"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #include <Common/EmissiveLights.glslh>

            // RayTracing::EmissiveMoveGpu: a static row whose transform moved, and its new one.
            struct EmissiveMove
            {
                uvec4 Row; // x: the static segment row
                vec4  ObjectToWorld[3];
            };

            layout(std430, binding = 0) buffer EmissiveSegmentsUB
            {
                EmissiveSegment emissiveSegments[];
            };
            layout(std430, binding = 1) readonly buffer EmissiveMovesUB
            {
                EmissiveMove emissiveMoves[];
            };

            PushConstant PushConstants
            {
                uvec4 MoveCounts; // x = moves
            };

            // One moved row per thread: its transform only (the rest of the row is the structure, unchanged).
            void main()
            {
                const uint move = gl_GlobalInvocationID.x;
                if (move >= MoveCounts.x)
                    return;
                const EmissiveMove m = emissiveMoves[move];
                emissiveSegments[m.Row.x].ObjectToWorld = m.ObjectToWorld;
            }
        }
    }
}
