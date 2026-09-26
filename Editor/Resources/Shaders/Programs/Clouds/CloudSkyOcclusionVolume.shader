// DesertAsset {"Kind":"Shader","Guid":"d253a17d785feff199efb2b680ace87c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "CloudSkyOcclusionVolume"
{
    Compute
    {
        // THE SKY-LIGHT OCCLUSION VOLUME. One column of the cloud field per invocation, integrated from
        // the top of the shell downward, writing at each of sixteen altitude slices the fraction of the
        // sky's mean radiance that survives everything above it.
        //
        // WHY IT EXISTS. The march's ambient term is multiplied by CloudAmbientOcclusion, which is a
        // function of the sample's own `Profile` — how deep it sits inside its OWN body — so no sample can
        // receive less than `mix(1, 0, strength)` of the whole sky however much OTHER cloud is stacked
        // over it. Р0 measured that as the largest discrepancy in this sky and its own §1 #2 as the reason
        // no dial fixes it (Docs/Clouds/DIAGNOSIS_CARTOON.md). The quantity that CAN fix it has a
        // different geometry — a hemisphere over the sample rather than a point inside a body — and
        // Common/CloudLighting.glslh carries the maths that turns this volume's number into it.
        //
        // WHAT ONE TEXEL MEANS: the DIFFUSE (cosine-weighted hemispherical) transmittance at that column,
        // at that altitude, in .r. Not the optical depth — the conversion is applied HERE, once per texel,
        // rather than at every one of the march's samples, and storing a quantity already in [0, 1] is
        // also what makes the trilinear filter between texels mean something.
        //
        // WHAT IT COSTS: resolution^2 columns of SLICES * 2 * CLOUD_SKY_OCCLUSION_HALF_SUBSTEPS field
        // samples each, and NO light march — which is what makes it a fraction of the shadow map beside
        // it rather than a second one. 128 x 128 columns x 64 samples is 1.05 M field samples against the
        // shadow map's 512 x 512 x 32 = 8.4 M. The measurement is in the report for Р4.
        //
        // WHERE IT RUNS. In-frame compute, outside any render pass, immediately before the march and after
        // the parameter buffer is written — the march READS what this writes, and both read the same
        // parameter block from the same single upload.

        #include <Common/CloudNoise.glslh>
        #include <Common/CloudGeometry.glslh>
        #include <Common/CloudLighting.glslh>

        // rgba16f, and only .r is written. Half precision on a quantity in [0, 1] resolves better than
        // one part in a thousand, which is finer than the 8-bit frame this multiplies into by a factor of
        // four. The other three channels are unwritten because Core::Formats::ImageFormat has no one- or
        // two-channel float format — a fact about the engine's formats, not a reserved slot.
        layout(binding = 0, rgba16f) restrict writeonly uniform image3D u_CloudSkyOcclusion;

        // The field, through the march's own bindings — one vocabulary for one field, exactly as
        // Programs/Clouds/CloudShadowMap.shader takes them. A column integrated from a different field
        // than the one the eye marches would darken clouds that are not there.
        Uniform(3) sampler3D u_CloudNoise;
        Uniform(10) sampler3D u_CloudNoise1;
        Uniform(11) sampler3D u_CloudNoise2;
        Uniform(12) sampler3D u_CloudNoise3;
        Uniform(7) sampler3D u_CloudModelling;
        Uniform(9) sampler3D u_CloudAuthoredAtlas;

        // The same four-way select the other two passes declare, repeated for the same reason:
        // Common/CloudField.glslh must stay free of samplers to remain compilable as C++ by its tests.
        vec4 CloudFetchNoise( int slot, vec3 p )
        {
            if ( slot == 1 )
                return texture( u_CloudNoise1, p );
            if ( slot == 2 )
                return texture( u_CloudNoise2, p );
            if ( slot == 3 )
                return texture( u_CloudNoise3, p );
            return texture( u_CloudNoise, p );
        }

        #define CLOUD_SAMPLE_NOISE(s, p) CloudFetchNoise((s), (p))
        #define CLOUD_SAMPLE_MODELLING(p) textureLod(u_CloudModelling, (p), 0.0f)
        #define CLOUD_SAMPLE_AUTHORED(p) textureLod(u_CloudAuthoredAtlas, (p), 0.0f)

        #define CLOUD_AUTHORED_BUFFER_BINDING 8
        #include <Common/CloudAuthored.glslh>

        #include <Common/CloudField.glslh>
        #include <Common/CloudParams.glslh>

        LocalSize(8, 8, 1);

        // SUB-SAMPLES PER HALF SLICE, so a slice costs four field samples and a column costs SLICES * 4.
        //
        // TWO, AND THE RELATION THAT FIXES IT: the sub-step is thickness / (SLICES * 2 * this), which at
        // the shipped 3.6 km congestus deck is 56 m — against the erosion's own wave of 235 m, measured
        // and printed by Desert/Tests/Engine/CloudField. A column integrated at a step coarser than the
        // structure it integrates does not merely lose detail, it loses ENERGY unpredictably, and this
        // number is what the layer's whole shaded side would then be scaled by.
        #define CLOUD_SKY_OCCLUSION_HALF_SUBSTEPS 2

        void main()
        {
            ivec3 size  = imageSize(u_CloudSkyOcclusion);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.z)
                return;

            CloudLayer       layer  = CloudUnpackLayer();
            CloudFieldParams params = CloudUnpackFieldParams();

            // A SHADOW RAY, even though it points at the sky rather than at the sun. The axis the flag
            // draws is what the march ACCUMULATES, not where it aims: this loop integrates optical depth
            // and stores a transmittance, exactly as the sun quadrature and the shadow map do, so an
            // author who cheapens the medium "where it is only attenuating" means this pass too. Calling
            // it a view ray because it is not aimed at a light would have left the one consumer that
            // touches nine tenths of every frame paying full price for a medium nobody looks at through
            // it.
            params.ShadowRay = CLOUD_RAY_SHADOW;

            float thicknessKm = max(u_CloudLayer.z, 1e-4f);
            float regionSideKm = 1.0f / max(u_CloudRegion.z, 1e-9f);

            // THE COLUMN'S PLACE IN THE WORLD, through the texel CENTRE. The consumer fetches this volume
            // trilinearly, so a column traced at a texel corner and read at its centre would be a
            // half-texel of horizontal displacement — 187 m at the shipped region, which is half a cloud.
            vec2 uv = (vec2(coord) + vec2(0.5f, 0.5f)) / vec2(float(size.x), float(size.z));

            // The region frame is the WIND-SHIFTED one (Common/CloudField.glslh subtracts the offset on
            // its way in), so the world position this column stands at is the region point PLUS the wind.
            // Constructing it this way rather than shifting the fetch is what puts the traced column and
            // the consumer's own CloudSkyOcclusionUvw on exactly the same texel however far the wind has
            // run — they are one mapping used in two directions rather than two mappings.
            float worldX = u_CloudRegion.x + uv.x * regionSideKm + u_CloudWind.x;
            float worldZ = u_CloudRegion.y + uv.y * regionSideKm + u_CloudWind.z;

            float extinction = max(u_CloudMarch.w, 0.0f);

            // The half-slice, as a fraction of the shell and as kilometres of path. RADIAL, and therefore
            // exactly the distance between two surfaces of constant height fraction — which is the
            // coordinate the consumer looks this volume up by, so producer and consumer share a vertical
            // axis with no geometry between them and the planet's curvature never enters.
            float halfSliceFraction = 0.5f / CLOUD_SKY_OCCLUSION_SLICES;
            float subFraction = halfSliceFraction / float(CLOUD_SKY_OCCLUSION_HALF_SUBSTEPS);
            float subKm = subFraction * thicknessKm;

            // Optical depth of everything ABOVE the point currently reached. Accumulated downward, so at
            // the moment a slice is written it holds exactly the material over that slice's centre.
            float tau = 0.0f;

            // The top of the shell downward. `heightFraction` walks with it and is passed to the field
            // rather than re-derived from a radius, because that is the coordinate the profile volume is
            // addressed by and a second derivation of it is the two-statements-of-one-fact defect.
            float heightFraction = 1.0f;

            for (int slice = int(CLOUD_SKY_OCCLUSION_SLICES) - 1; slice >= 0; --slice)
            {
                // The UPPER half of this slice: material that is above the slice's centre.
                for (int s = 0; s < CLOUD_SKY_OCCLUSION_HALF_SUBSTEPS; ++s)
                {
                    float midFraction = heightFraction - 0.5f * subFraction;
                    vec3  fieldPos    = vec3(worldX, midFraction * thicknessKm, worldZ);

                    CloudFieldSample field = SampleCloudField(params, midFraction, fieldPos);
                    if (field.Profile > 0.0f)
                    {
                        float density = CloudSampleDensity(params, field, fieldPos);
                        tau += density * extinction *
                               CloudSampleExtinctionFactor(params, field, fieldPos) * subKm;
                    }

                    heightFraction -= subFraction;
                }

                imageStore(u_CloudSkyOcclusion, ivec3(coord.x, slice, coord.y),
                           vec4(CloudSkyDiffuseTransmittance(tau), 0.0f, 0.0f, 0.0f));

                // The LOWER half, which belongs to the slices under this one and not to it.
                for (int s = 0; s < CLOUD_SKY_OCCLUSION_HALF_SUBSTEPS; ++s)
                {
                    float midFraction = heightFraction - 0.5f * subFraction;
                    vec3  fieldPos    = vec3(worldX, midFraction * thicknessKm, worldZ);

                    CloudFieldSample field = SampleCloudField(params, midFraction, fieldPos);
                    if (field.Profile > 0.0f)
                    {
                        float density = CloudSampleDensity(params, field, fieldPos);
                        tau += density * extinction *
                               CloudSampleExtinctionFactor(params, field, fieldPos) * subKm;
                    }

                    heightFraction -= subFraction;
                }
            }
        }
    }
}
