// DesertAsset {"Kind":"Shader","Guid":"b7e41c2d95a84f6e8a0d3c51f29e7a64","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCENE'S EMISSIVE TRIANGLES (Common/EmissiveLights.glslh; RayTracing::EmissiveLightsRenderer): "Build" expands
// the emissive segments of the list into world triangles and their powers, "Reduce" sums the power tree level by
// level, "Move" writes a moved row's transform - per part of the list: the static part whole when its structure
// changes (an emitter added, removed or re-materialled), and only the moved rows' leaves and their path to the root
// when only transforms moved (kept across frames otherwise); the dynamic part (the skinned primitives' segments,
// read from the skin cache's positions) whole every frame. Build and Reduce write the RUNS of nodes the CPU names
// (EmissiveNodeRun, RayTracing::EmissiveDirtyRuns): the whole tree is one run per level. "Clusters", "GridBounds"
// and "Cells" build the list's selection grid from both trees (Common/EmissiveLightsSampling.glslh) in the frames
// the static part was rebuilt or moved and in every frame with a dynamic part.
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

            // The triangle's emitted radiance in the row's Edge2Segment.w: a shared 8-bit exponent (bias 136 with the
            // mantissas) over three 8-bit mantissas, the HDR range of a float at 1/256 of the brightest channel.
            // Decoded by EmissiveLightsRadiance (Common/EmissiveLightsSampling.glslh); 0 = no emission.
            uint EmissivePackRadiance(vec3 radiance)
            {
                const float brightest = max(radiance.r, max(radiance.g, radiance.b));
                if (!(brightest > 1e-30))
                    return 0u;
                int exponent;
                frexp(brightest, exponent); // brightest = f x 2^exponent, f in [0.5, 1)
                const int biased = exponent + 128;
                if (biased <= 0)
                    return 0u;
                const uvec3 m = uvec3(min(round(radiance * exp2(float(8 - exponent))), vec3(255.0)));
                return m.r | (m.g << 8u) | (m.b << 16u) | (uint(min(biased, 255)) << 24u);
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
                // The triangle's ONE radiance: the material's colour x intensity times the emissive map's rgb
                // average over the triangle; its power (the tree's weight) is that same rgb's luminance x area.
                const vec3            emitted     = s.Radiance.rgb * EmissiveTexelAverage(s, v0, v1, v2);
                const float           power       = dot(emitted, vec3(0.2126, 0.7152, 0.0722)) * area;
                EmissiveTriangle      t;
                t.P0Area                  = vec4(p0, area);
                t.Edge1Power              = vec4(e1, power);
                t.Edge2Segment            = vec4(e2, uintBitsToFloat(EmissivePackRadiance(emitted)));
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

    // THE SELECTION GRID (EmissiveGridUB, rows in Common/EmissiveLightsSampling.glslh), built from the trees above:
    // "Clusters" bounds each cluster (one node of a part's tree at the cluster level), "GridBounds" lays the cells
    // over the clusters with power, "Cells" keeps each cell's strongest clusters and their CDF. The layout numbers
    // (header rows, slots, remainder floor) arrive as push constants from RayTracing::EmissiveLightsFrame.hpp.

    Pass "Clusters"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #include <Common/EmissiveLights.glslh>
            #include <Common/EmissiveLightsGridBuild.glslh>

            layout(std430, binding = 0) readonly buffer EmissiveTrianglesUB
            {
                EmissiveTriangle emissiveTriangles[];
            };
            layout(std430, binding = 1) readonly buffer EmissiveDynamicTrianglesUB
            {
                EmissiveTriangle emissiveDynamicTriangles[];
            };
            layout(std430, binding = 2) writeonly buffer EmissiveClustersUB
            {
                EmissiveCluster emissiveClusters[];
            };

            PushConstant PushConstants
            {
                uvec4 StaticPart;  // x = triangles, y = cluster level, z = clusters
                uvec4 DynamicPart; // the same for the dynamic part (its clusters follow the static ones)
            };

            EmissiveTriangle EmissivePartTriangle(bool dynamicPart, uint triangle)
            {
                return dynamicPart ? emissiveDynamicTriangles[triangle] : emissiveTriangles[triangle];
            }

            // One cluster per thread: its triangles with power, their power-weighted centroid and the farthest
            // corner from it. A cluster with no power is written as zeros (never a candidate).
            void main()
            {
                const uint cluster = gl_GlobalInvocationID.x;
                if (cluster >= StaticPart.z + DynamicPart.z)
                    return;
                const bool  dynamicPart = cluster >= StaticPart.z;
                const uvec4 part        = dynamicPart ? DynamicPart : StaticPart;
                const uint  node        = dynamicPart ? cluster - StaticPart.z : cluster;
                const uint  first       = node << part.y;
                const uint  end         = min(first + (1u << part.y), part.x);
                float       power       = 0.0;
                vec3        weighted    = vec3(0.0);
                for (uint triangle = first; triangle < end; ++triangle)
                {
                    const EmissiveTriangle t = EmissivePartTriangle(dynamicPart, triangle);
                    if (t.Edge1Power.w > 0.0)
                    {
                        power    += t.Edge1Power.w;
                        weighted += t.Edge1Power.w * (t.P0Area.xyz + (t.Edge1Power.xyz + t.Edge2Segment.xyz) / 3.0);
                    }
                }
                EmissiveCluster result;
                result.Sphere = vec4(0.0);
                result.Power  = vec4(0.0);
                if (power > 0.0)
                {
                    const vec3 centre = weighted / power;
                    float      radius = 0.0;
                    for (uint triangle = first; triangle < end; ++triangle)
                    {
                        const EmissiveTriangle t = EmissivePartTriangle(dynamicPart, triangle);
                        if (t.Edge1Power.w > 0.0)
                            radius = max(radius, max(distance(centre, t.P0Area.xyz),
                                                     max(distance(centre, t.P0Area.xyz + t.Edge1Power.xyz),
                                                         distance(centre, t.P0Area.xyz + t.Edge2Segment.xyz))));
                    }
                    result.Sphere = vec4(centre, radius);
                    result.Power  = vec4(power, 0.0, 0.0, 0.0);
                }
                emissiveClusters[cluster] = result;
            }
        }
    }

    Pass "GridBounds"
    {
        Compute
        {
            LocalSize(1, 1, 1);

            #include <Common/EmissiveLightsGridBuild.glslh>

            layout(std430, binding = 0) readonly buffer EmissiveClustersUB
            {
                EmissiveCluster emissiveClusters[];
            };
            layout(std430, binding = 1) writeonly buffer EmissiveGridUB
            {
                vec4 emissiveGrid[];
            };

            PushConstant PushConstants
            {
                uvec4 Clusters; // x = clusters of the static part, y = of both parts, z = static level, w = dynamic level
                uvec4 Layout;   // x = cells per axis at most
            };

            // One thread: the box over the spheres of the clusters with power (none: a point at the origin), the
            // cells per axis and their size, the header rows.
            void main()
            {
                vec3 lo = vec3(3.402823e38);
                vec3 hi = vec3(-3.402823e38);
                for (uint cluster = 0u; cluster < Clusters.y; ++cluster)
                {
                    const EmissiveCluster c = emissiveClusters[cluster];
                    if (c.Power.x > 0.0)
                    {
                        lo = min(lo, c.Sphere.xyz - vec3(c.Sphere.w));
                        hi = max(hi, c.Sphere.xyz + vec3(c.Sphere.w));
                    }
                }
                if (lo.x > hi.x)
                {
                    lo = vec3(0.0);
                    hi = vec3(0.0);
                }
                const vec3  extent = hi - lo;
                const uvec3 dims   = uvec3(clamp(ceil(extent / kEmissiveGridCellTarget), vec3(1.0), vec3(float(Layout.x))));
                const vec3  size   = mix(vec3(kEmissiveGridCellTarget), extent / vec3(dims), greaterThan(extent, vec3(0.0)));
                emissiveGrid[0] = vec4(lo, 0.0);
                emissiveGrid[1] = vec4(size, 0.0);
                emissiveGrid[2] = vec4(uintBitsToFloat(dims), uintBitsToFloat(Clusters.z));
                emissiveGrid[3] = vec4(uintBitsToFloat(Clusters.w), uintBitsToFloat(Clusters.x), 0.0, 0.0);
            }
        }
    }

    Pass "Cells"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #include <Common/EmissiveLights.glslh>
            #include <Common/EmissiveLightsGridBuild.glslh>

            layout(std430, binding = 0) readonly buffer EmissiveClustersUB
            {
                EmissiveCluster emissiveClusters[];
            };
            layout(std430, binding = 1) buffer EmissiveGridUB
            {
                vec4 emissiveGrid[];
            };

            PushConstant PushConstants
            {
                uvec4 Layout;  // x = header rows, y = slots per cell, z = clusters of both parts
                vec4  Weights; // x = the least weight of the remainder slot
            };

            uint EmissiveSlotRow(uint cell, uint slot)
            {
                return Layout.x + cell * (Layout.y / 2u) + slot / 2u;
            }

            vec2 EmissiveReadSlot(uint cell, uint slot)
            {
                const vec4 row = emissiveGrid[EmissiveSlotRow(cell, slot)];
                return (slot & 1u) == 0u ? row.xy : row.zw;
            }

            void EmissiveWriteSlot(uint cell, uint slot, vec2 entry)
            {
                const uint row = EmissiveSlotRow(cell, slot);
                if ((slot & 1u) == 0u)
                    emissiveGrid[row].xy = entry;
                else
                    emissiveGrid[row].zw = entry;
            }

            // One cell per thread. A cluster's weight here is its power / max(gap to its sphere ^ 2, the cell's half
            // diagonal ^ 2), gap from the cell's centre. The slots-1 strongest are held IN the cell's own slots while
            // the clusters are scanned (x = cluster, y = weight; the weakest replaced, ties keep the earlier one), then
            // rewritten as CDF bounds: the held clusters share 1 - remainder in proportion to their weights, the
            // remainder (the weight of the clusters not held, at least Weights.x; 1 with no weight at all) is last.
            void main()
            {
                const uvec3 dims = floatBitsToUint(emissiveGrid[2].xyz);
                const uint  cell = gl_GlobalInvocationID.x;
                if (cell >= dims.x * dims.y * dims.z)
                    return;
                const uvec3 coords     = uvec3(cell % dims.x, (cell / dims.x) % dims.y, cell / (dims.x * dims.y));
                const vec3  size       = emissiveGrid[1].xyz;
                const vec3  centre     = emissiveGrid[0].xyz + (vec3(coords) + 0.5) * size;
                const float nearest    = 0.25 * dot(size, size);
                const uint  candidates = Layout.y - 1u;
                uint        held       = 0u;
                uint        weakest    = 0u;
                float       weakestW   = 0.0;
                float       total      = 0.0;
                for (uint cluster = 0u; cluster < Layout.z; ++cluster)
                {
                    const EmissiveCluster c = emissiveClusters[cluster];
                    if (!(c.Power.x > 0.0))
                        continue;
                    const float gap    = max(distance(centre, c.Sphere.xyz) - c.Sphere.w, 0.0);
                    const float weight = c.Power.x / max(gap * gap, nearest);
                    total += weight;
                    if (held < candidates)
                    {
                        EmissiveWriteSlot(cell, held, vec2(uintBitsToFloat(cluster), weight));
                        if (held == 0u || weight < weakestW)
                        {
                            weakest  = held;
                            weakestW = weight;
                        }
                        ++held;
                    }
                    else if (weight > weakestW)
                    {
                        EmissiveWriteSlot(cell, weakest, vec2(uintBitsToFloat(cluster), weight));
                        weakestW = weight;
                        for (uint slot = 0u; slot < held; ++slot)
                        {
                            const float w = EmissiveReadSlot(cell, slot).y;
                            if (w < weakestW)
                            {
                                weakest  = slot;
                                weakestW = w;
                            }
                        }
                    }
                }
                float top = 0.0;
                for (uint slot = 0u; slot < held; ++slot)
                    top += EmissiveReadSlot(cell, slot).y;
                const float remainder = total > 0.0 ? max(Weights.x, (total - top) / total) : 1.0;
                // Each held cluster once, its bound the running sum: nondecreasing by construction, held to
                // 1 - remainder against rounding, so the remainder's width is never below its floor.
                const float heldTop  = 1.0 - remainder;
                float       bound     = 0.0;
                for (uint slot = 0u; slot < held; ++slot)
                {
                    const vec2 entry = EmissiveReadSlot(cell, slot);
                    bound = slot + 1u == held ? heldTop : min(bound + heldTop * entry.y / top, heldTop);
                    EmissiveWriteSlot(cell, slot, vec2(entry.x, bound));
                }
                // Unused slots: zero width at the last bound (never picked, no pdf).
                for (uint slot = held; slot < candidates; ++slot)
                    EmissiveWriteSlot(cell, slot, vec2(uintBitsToFloat(kEmissiveNone), bound));
                EmissiveWriteSlot(cell, candidates, vec2(uintBitsToFloat(kEmissiveNone), 1.0));
            }
        }
    }
}
