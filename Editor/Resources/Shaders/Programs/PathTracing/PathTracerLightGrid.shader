// DesertAsset {"Kind":"Shader","Guid":"25ac1e7117ba495bafc23a28d49822eb","Versions":{"SHDR":1},"Dependencies":[]}
// THE PATH TRACER'S LIGHT GRID (Common/PathTracingLightGrid.glslh; PathTracing.LightSampling 1): one invocation per
// slot of every cell fills that slot by weighted reservoir sampling over LightCounts.z candidates drawn uniformly
// from the local lights, each weighed by its luminance at the point of the cell closest to it (NVIDIA RTXDI ReGIR,
// ReGIR_GridFill: the target there is the light's power times its falloff to the cell). Run once per traced frame
// (System::PathTracer::AddLightGridPass) with a new seed, so the slots of successive frames are independent and the
// accumulated image converges to the all-lights estimator's (PathTracing.LightSampling 0).
Shader "PathTracerLightGrid"
{
    Pass "Build"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #include <Mesh/LightSources.glslh>
            #include <Common/PathTracingRandom.glslh>
            #include <Common/PathTracingLightGrid.glslh>
            #include <Common/RisReservoir.glslh>

            layout(std430, binding = 0) readonly buffer PointLightsUB
            {
                PointLight pointLights[];
            };
            layout(std430, binding = 1) readonly buffer SpotLightsUB
            {
                SpotLight spotLights[];
            };
            // Per slot: x = the local light (kPathTracingLightGridNone: empty), y = its W (floatBitsToUint).
            layout(std430, binding = 2) writeonly buffer PathTracingLightGridUB
            {
                uvec2 lightGrid[];
            };

            PushConstant PushConstants
            {
                vec4  LightGrid;     // xyz = the grid's min corner (cm), w = the cell side (cm)
                uvec4 LightGridSize; // xyz = cells along x, y, z, w = slots per cell
                uvec4 LightCounts;   // x = point lights, y = spot lights, z = candidates per slot, w = the frame's seed
            };

            // The cell is widened by this many centimetres each side: a shading point is filed into the cell by a
            // floor of its scaled position, so one rounded across a face still lies inside the bounds weighed here.
            const float kPathTracingLightGridCellMargin = 1.0;

            // A light's luminance at the point of [cellMin, cellMax] closest to it: an upper bound of its luminance
            // anywhere in the cell (LightFalloffFactor never rises with distance), the cone of a spot light left out.
            float PathTracingLightGridTarget(uint light, vec3 cellMin, vec3 cellMax)
            {
                vec3  position;
                vec3  power;
                float minRadius;
                float reach;
                int   falloff;
                if (light < LightCounts.x)
                {
                    const PointLight source = pointLights[light];
                    position  = source.position;
                    power     = source.color * source.intensity;
                    minRadius = source.minRadius;
                    reach     = source.radius;
                    falloff   = source.falloff;
                }
                else
                {
                    const SpotLight source = spotLights[light - LightCounts.x];
                    position  = source.position;
                    power     = source.color * source.intensity;
                    minRadius = source.minRadius;
                    reach     = source.range;
                    falloff   = source.falloff;
                }
                const float dist = distance(position, clamp(position, cellMin, cellMax));
                return max(PathTracingLuminance(power), 0.0) * LightFalloffFactor(dist, minRadius, reach, falloff);
            }

            void main()
            {
                const uint cells = LightGridSize.x * LightGridSize.y * LightGridSize.z;
                const uint slot  = gl_GlobalInvocationID.x;
                if (slot >= cells * LightGridSize.w)
                    return;
                const uint  cell    = slot / LightGridSize.w;
                const uvec3 index   = uvec3(cell % LightGridSize.x, (cell / LightGridSize.x) % LightGridSize.y,
                                            cell / (LightGridSize.x * LightGridSize.y));
                const vec3  cellMin = LightGrid.xyz + vec3(index) * LightGrid.w - kPathTracingLightGridCellMargin;
                const vec3  cellMax = cellMin + LightGrid.w + 2.0 * kPathTracingLightGridCellMargin;

                const uint lights     = LightCounts.x + LightCounts.y;
                const uint candidates = LightCounts.z;
                uint       rng        = PathTracingHash(slot + PathTracingHash(LightCounts.w));
                // Initial RIS (Common/RisReservoir.glslh): `candidates` uniform draws over the lights, source
                // density 1 / lights, so M = candidates.
                RisMerge stream = RisBeginMerge();
                for (uint k = 0u; k < candidates; ++k)
                {
                    const uint light = min(uint(PathTracingRandom(rng) * float(lights)), lights - 1u);
                    RisStreamCandidate(stream, int(light), PathTracingLightGridTarget(light, cellMin, cellMax),
                                       float(lights), PathTracingRandom(rng));
                }
                const RisReservoir chosen = RisEndStream(stream, float(candidates));
                lightGrid[slot] = chosen.Light >= 0 ? uvec2(uint(chosen.Light), floatBitsToUint(chosen.W))
                                                    : uvec2(kPathTracingLightGridNone, floatBitsToUint(0.0));
            }
        }
    }
}
