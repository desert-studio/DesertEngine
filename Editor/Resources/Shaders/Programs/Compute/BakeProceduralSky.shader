// DesertAsset {"Kind":"Shader","Guid":"bfe8d1c7d819b65096435cd960508732","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BakeProceduralSky"
{
    Compute
    {
        // Renders the procedural sky STRAIGHT INTO THE FACES of the sky light's radiance cube (imageCube,
        // RGBA32F, mip 0, at the capture's face -- 128 by default), as UE's real-time sky light capture renders
        // the sky material and the volumetric clouds per face at its capture resolution. There is no panorama
        // and no resample: the cube's mips, the exact irradiance sum and the GGX prefilter follow
        // (Graphic::System::SkyCapture). A dispatch covers BakeFace.x and the faces after it, one per z, so a
        // time-sliced recapture renders one band of rows of one face per frame (BakeFace.y, the first row). Both sky models
        // are the SAME evaluations the screen pass runs (Common/Atmosphere.glslh for the gradient,
        // Common/SkyScattering.glslh's integrator for the physical atmosphere) — the baked environment
        // and the visible sky cannot drift apart.
        //
        // AND THE CLOUDS ARE IN IT. The layer is marched into this capture by the same field, the same
        // lighting and the same quadrature the screen pass uses (Common/CloudField.glslh,
        // Common/CloudLighting.glslh) — not by an analytic dome standing beside them, which would be a
        // SECOND model of the clouds and therefore a mirror that drifts. This project has paid for that
        // shape once already, in the grey-clouds defect, and the paragraph above is the promise the cloud
        // term had to keep as well.
        //
        // The physical branch marches the integrator directly per texel (32 samples, the Sky-View
        // budget) instead of sampling the per-view Sky-View LUT: the capture is anchored at a fixed point,
        // not at the camera, and must not change when the camera does — its cadence is driven by the
        // sun and by the cloud settings alone. Deliberate divergences from the screen pass, all of them
        // invisible to the IBL or unavoidable, and all stated rather than discovered:
        //   * anchored at a fixed 0.2 km — the sky varies imperceptibly across ground-level altitudes,
        //     and anchoring at the camera would demand a rebake on every elevator ride;
        //   * NO analytic sun disc — the disc's energy over a face texel is the whole direct sun
        //     illuminance, and the directional light already delivers exactly that to every surface;
        //     baking it too would double-count the sun in the irradiance cube;
        //   * the cloud march's aerial perspective is INTEGRATED HERE rather than fetched from the
        //     camera aerial-perspective volume. That volume is the camera's froxels and the capture
        //     is anchored elsewhere; the integrand is Common/SkyScattering.glslh's SkyApIntegrateSegment either
        //     way, which is the same text the volume is filled from, so this is one model evaluated
        //     exactly instead of one model evaluated through a 32x32x16 interpolation;
        //   * the march is DITHERED ON THE TEXEL and not on a frame index. There is no temporal
        //     accumulation to converge a moving pattern here, and a fixed start offset would draw the
        //     step planes as concentric shells; a spatial hash makes the residual decorrelated noise,
        //     which the irradiance sum and the prefilter integrate away and which keeps a recapture of
        //     unchanged settings byte-identical to the one before it;
        //   * and NOT a cheaper cloud march: the capture marches at the screen's sample counts, because the
        //     multiple-scattering octaves make a cloud marched in longer steps brighter (measured, see
        //     Graphic::System::kSkyCaptureBandsPerFace); the cost is spread over frames, a band of rows of
        //     one face per frame, instead.
        //
        // Each face texel's direction is PanoramaToCubemap's face table (the one the .hdr path's cubes are
        // laid out by, and the one every cube in the engine is sampled with), at the texel's centre.

        #include <Common/Atmosphere.glslh>
        #include <Common/SkyMedium.glslh>

        layout(binding = 0, rgba32f) restrict writeonly uniform imageCube outputFaces;

        // The SAME sky parameter buffer the screen pass reads — not a second hand-packed mirror of it. The
        // bake used to receive these as a push-constant block, which meant two layouts to keep in step and
        // a silent corruption of everything after the first field that fell out of order.
        //
        // The binding number is bound explicitly from C++ (ComputePipeline::SetStorageBuffer) and must stay
        // equal to Graphic::kSkyPayloadBinding and to the number the graphics sky shader declares.
        ReadBuffer(1) SkyBuffer
        {
            vec4 SkyPackedData[SKY_PACKED_VEC4_COUNT];
        };

        // The cached atmosphere LUTs (Graphic::kSkyTransmittanceLutBinding / kSkyMultiScatterLutBinding).
        // SkyboxRenderer dispatches them immediately before a physical bake; on the gradient model the
        // C++ side binds fallbacks and the physical branch below never runs.
        // Every input of this bake is a separate texture read through one of two shared states, the ones the
        // screen passes read the same images with: GlobalFilterSampler (Graphic::kSkyBakeGlobalFilterSamplerBinding)
        // for the LUTs and the distant sky light, LinearRepeatSampler (kSkyBakeLinearRepeatSamplerBinding) for
        // the cloud volumes and maps.
        Uniform(18) sampler GlobalFilterSampler;
        Uniform(17) sampler LinearRepeatSampler;

        Uniform(2) texture2D TransmittanceLutTexture;
        #define TransmittanceLutTexture sampler2D(TransmittanceLutTexture, GlobalFilterSampler)
        Uniform(3) texture2D MultiScatterLutTexture;
        #define MultiScatterLutTexture sampler2D(MultiScatterLutTexture, GlobalFilterSampler)

        // The same read-side mappings the SkyViewLut fill uses: raw uv for the transmittance LUT,
        // texel-centre remap for the multi-scatter LUT — each the exact inverse of its write side.
        vec3 SkySampleSunTransmittanceLut(SkyAtmParams atm, float radiusKm, float sunZenithCos)
        {
            vec2 uv = SkyTransmittanceLutUvFromParams(atm.BottomRadiusKm, atm.TopRadiusKm, radiusKm,
                                                      sunZenithCos);
            return texture(TransmittanceLutTexture, uv).rgb;
        }

        vec3 SkySampleMultiScatterLut(SkyAtmParams atm, float radiusKm, float sunZenithCos)
        {
            vec2 unit = SkyMultiScatterUnitFromParams(atm.BottomRadiusKm, atm.TopRadiusKm, radiusKm,
                                                      sunZenithCos);
            vec2 uv   = vec2(SkyUnitToTexelUv(unit.x, 32.0f), SkyUnitToTexelUv(unit.y, 32.0f));
            return texture(MultiScatterLutTexture, uv).rgb;
        }

        #define SKY_SCATTERING_SUN_TRANSMITTANCE(atm, radiusKm, sunZenithCos) SkySampleSunTransmittanceLut(atm, radiusKm, sunZenithCos)
        #define SKY_SCATTERING_MULTI_SCATTER(atm, radiusKm, sunZenithCos) SkySampleMultiScatterLut(atm, radiusKm, sunZenithCos)

        #include <Common/SkyScattering.glslh>

        // ---- THE CLOUD LAYER ------------------------------------------------------------------------
        //
        // Everything below to the end of the includes is the march's own resource list, bound on exactly
        // the terms Programs/Clouds/CloudRaymarch.shader binds it on: ALWAYS, fallbacks included. A
        // declared sampler with no image is an INVALID descriptor set rather than an unused one, and this
        // backend answers an invalid set by skipping the whole dispatch — which here would lose not the
        // clouds but the entire environment, with nothing in the log.

        #include <Common/CloudNoise.glslh>
        #include <Common/CloudGeometry.glslh>
        #include <Common/CloudLighting.glslh>
        #include <Common/CloudAerial.glslh>

        // The four noise volumes a layer's species can name, deduplicated by
        // Graphic::ResolveCloudNoiseVolumes exactly as they are for the screen march. Separate bindings
        // and not an array: this engine's reflection refuses an array of descriptors.
        Uniform(5) texture3D CloudNoiseTexture;
        #define CloudNoiseTexture sampler3D(CloudNoiseTexture, LinearRepeatSampler)
        Uniform(6) texture3D CloudNoise1Texture;
        #define CloudNoise1Texture sampler3D(CloudNoise1Texture, LinearRepeatSampler)
        Uniform(7) texture3D CloudNoise2Texture;
        #define CloudNoise2Texture sampler3D(CloudNoise2Texture, LinearRepeatSampler)
        Uniform(8) texture3D CloudNoise3Texture;
        #define CloudNoise3Texture sampler3D(CloudNoise3Texture, LinearRepeatSampler)

        // The procedural MODELLING VOLUME (256 x 32 x 256 RGBA8) this view's clouds are shaped by, and the
        // sculpted hero-cloud ATLAS beside it. Both borrowed from the VolumetricCloudRenderer of the same
        // SceneRenderer, so the capture is baked from the field the frame is about to march.
        Uniform(9) texture3D CloudModellingTexture;
        #define CloudModellingTexture sampler3D(CloudModellingTexture, LinearRepeatSampler)
        // The WORLD WEATHER map (Graphic::kCloudFarWeatherBinding): the remap Common/CloudField.glslh makes
        // of the baked profile by CloudWeather.
        // Always bound, on the terms every sampler here is.
        Uniform(14) texture2D CloudFarWeatherTexture;
        #define CloudFarWeatherTexture sampler2D(CloudFarWeatherTexture, LinearRepeatSampler)
        Uniform(15) texture2D CloudLayoutPatternTexture;
        #define CloudLayoutPatternTexture sampler2D(CloudLayoutPatternTexture, LinearRepeatSampler)
        Uniform(16) texture2D CloudLayoutMaskTexture;
        #define CloudLayoutMaskTexture sampler2D(CloudLayoutMaskTexture, LinearRepeatSampler)
        Uniform(10) texture3D CloudAuthoredAtlasTexture;
        #define CloudAuthoredAtlasTexture sampler3D(CloudAuthoredAtlasTexture, LinearRepeatSampler)

        // The sky's DISTANT SKY LIGHT: one texel holding the average radiance of the whole sky. It is the
        // physical model's ambient and CloudAmbient.w decides whether it is read, which is the same gate
        // the screen march applies.
        Uniform(12) texture2D DistantSkyLightTexture;
        #define DistantSkyLightTexture sampler2D(DistantSkyLightTexture, GlobalFilterSampler)

        // THE SKY-LIGHT OCCLUSION VOLUME, written by the previous frame's
        // Programs/Clouds/CloudSkyOcclusionVolume.shader over the modelling volume's own region. Read on
        // exactly the screen march's gate, so a deck that shades its own underside on screen shades it in
        // the bake too; without it the baked dome is brighter than the visible one by the whole of the
        // layer's self-occlusion.
        Uniform(13) texture3D CloudSkyOcclusionVolumeTexture;
        #define CloudSkyOcclusionVolumeTexture sampler3D(CloudSkyOcclusionVolumeTexture, LinearRepeatSampler)

        // The four-way select, a compare chain because a sampler is not indexable in this dialect. The CPU
        // deduplicates the slots, so the ordinary sky sends 0 for every species and this is one fetch.
        vec4 CloudFetchNoise( int slot, vec3 p )
        {
            if ( slot == 1 )
                return texture( CloudNoise1Texture, p );
            if ( slot == 2 )
                return texture( CloudNoise2Texture, p );
            if ( slot == 3 )
                return texture( CloudNoise3Texture, p );
            return texture( CloudNoiseTexture, p );
        }

        #define CLOUD_SAMPLE_NOISE(s, p) CloudFetchNoise((s), (p))
        // textureLod AND NOT texture: a compute shader has no derivatives, so the implicit level of detail
        // is undefined. Both volumes have one level, so every implementation happens to pick it — but
        // "happens to" is the state three other sites in this engine were found in.
        #define CLOUD_SAMPLE_MODELLING(p) textureLod(CloudModellingTexture, (p), 0.0f)
        #define CLOUD_SAMPLE_WEATHER(uv) textureLod(CloudFarWeatherTexture, (uv), 0.0f).r
        #define CLOUD_SAMPLE_LAYOUT_PATTERN(uv) textureLod(CloudLayoutPatternTexture, (uv), 0.0f)
        #define CLOUD_SAMPLE_LAYOUT_MASK(uv) textureLod(CloudLayoutMaskTexture, (uv), 0.0f).r
        #define CLOUD_SAMPLE_AUTHORED(p) textureLod(CloudAuthoredAtlasTexture, (p), 0.0f)

        // Slot A's instance list, included BEFORE the seam because the seam's authored producer reads the
        // block this declares and GLSL has no forward declarations.
        #define CLOUD_AUTHORED_BUFFER_BINDING 11
        #include <Common/CloudAuthored.glslh>

        #include <Common/CloudField.glslh>

        // FOUR AND NOT ONE. Graphic::kSkyPayloadBinding is 1 and so is the cloud block's default, and this
        // is the one pass in the engine that reads both — two blocks on one descriptor is not a
        // diagnosable failure, it is one of them reading the other's bytes. Common/CloudParams.glslh takes
        // the override; Graphic::kSkyBakeCloudParamsBinding is the C++ half of this number.
        #define CLOUD_PARAMS_BINDING 4
        #include <Common/CloudParams.glslh>

        PushConstant SkyBakePush
        {
            // x = 1 when this view has a cloud layer to march and every cloud resource above is real; 0
            //     when the capture is the sky alone, in which case the whole march below is skipped and
            //     the result is bit for bit what it was before clouds reached this shader.
            // y = 1 when the sky-light occlusion volume was written and may be read (the screen march's
            //     own gate, carried here for the same reason: the component's flag can be on while the
            //     dispatch did not happen, and reading a volume nobody wrote shades the sky with
            //     uninitialised device memory).
            // z = the atmosphere's Aerial Perspective Start Depth in kilometres. It reaches the screen
            //     march baked into the aerial-perspective volume; this pass integrates the air itself and
            //     therefore has to be told, or a scene that pushes the haze out would still see it here.
            // w = 1 when CloudSunColour.rgb carries the sun's OUTER-SPACE illuminance and this pass must
            //     apply the atmosphere's transmittance at each cloud sample's own altitude itself. It is
            //     the SCREEN march's gate again (CloudPush::Frame.y), carried because both passes light
            //     the field from ONE packed block: with the flag on and this lane at 0 the capture would
            //     be lit by the sun as seen from space, which at a low sun is several times the light the
            //     scene is exposed for and reads as an exposure bug rather than a missing multiply.
            //     No binding travels with it — this pass already samples the SKY's own transmittance LUT
            //     at binding 2 and already derives the shell's radii from the sky block it reads.
            vec4 BakeClouds;
            // x = the first cube face this dispatch renders; gl_GlobalInvocationID.z counts from it.
            // y = the first row; gl_GlobalInvocationID.y counts from it (a band of a time-sliced capture).
            uvec4 BakeFace;
        };

        const int   kBakeSampleCount      = 32;   // the Sky-View budget, per face texel
        const float kBakeAnchorAltitudeKm = 0.2f; // the fixed viewpoint (see the header)

        SkyAtmParams MakeBakeAtmosphere(SkyPacked s)
        {
            return SkyMakeAtmParams(UnpackMediumRayleigh(s), UnpackMediumMie(s),
                                    UnpackMediumMieAbsorption(s), UnpackMediumOzone(s),
                                    UnpackMediumGround(s), UnpackMediumTentPlanet(s));
        }

        vec3 EvaluatePhysicalSkyForBake(SkyPacked s, SkyAtmParams atm, vec3 dir)
        {
            vec3 originKm       = vec3(0.0f, atm.BottomRadiusKm + kBakeAnchorAltitudeKm, 0.0f);
            vec3 sunDir         = UnpackSunDirection(s);
            vec3 sunIlluminance = UnpackAtmosphereLightIlluminance(s);

            SkyScatterResult result = SkyIntegrateScatteredLuminance(atm, originKm, dir, sunDir,
                                                                     sunIlluminance, kBakeSampleCount);

            vec3 sky = result.Luminance * UnpackSkyAndAerialPerspectiveLuminanceFactor(s);

            // Below the horizon: the lit ground under the marched air — the same v1 formula the screen
            // pass applies, with the view transmittance coming from this texel's own march.
            float r0 = originKm.y;
            if (SkyIntersectsGround(r0, dir.y, atm.BottomRadiusKm))
            {
                float sunZenithCos = clamp(sunDir.y, -1.0f, 1.0f);
                vec3  sunT         = texture(TransmittanceLutTexture,
                                             SkyTransmittanceLutUvFromParams(
                                                  atm.BottomRadiusKm, atm.TopRadiusKm,
                                                  atm.BottomRadiusKm + SKY_PLANET_RADIUS_OFFSET_KM,
                                                  sunZenithCos)).rgb;
                float viewT = (result.Transmittance.r + result.Transmittance.g +
                               result.Transmittance.b) / 3.0f;
                sky += SkyGroundLuminance(atm, sunIlluminance, sunT, sunZenithCos) * viewT;
            }

            // The screen-pixel tint applies here too: the reflection of the sky must be the sky.
            return sky * UnpackSkyLuminanceFactor(s);
        }

        // What the cloud layer contributes along one capture ray: premultiplied radiance and the
        // transmittance of the layer, exactly the two lanes the screen march writes into its RGBA16F
        // target — so the composite over the sky is the same premultiplied over-operator the frame uses.
        struct CloudBakeResult
        {
            vec3  Luminance;
            float Transmittance;
        };

        // THE SUN A CLOUD SAMPLE SEES, and it is Programs/Clouds/CloudRaymarch.shader's function of the
        // same name with one difference: the shell's two radii come from the sky block this pass already
        // reads rather than from a push constant, because this pass binds the sky and that one does not.
        // The arithmetic between them is SkySunAtAltitude's, once, in Common/SkyScattering.glslh.
        //
        // WHY THIS PASS NEEDS IT AT ALL. The capture is marched from the SAME packed block the screen
        // march reads, and ECS::VolumetricCloudData::PerSampleAtmosphereTransmittance changes what
        // CloudSunColour MEANS rather than adding a field beside it. A bake that ignored the lane would
        // light every cloud in the IBL chain with the sun as seen from space.
        vec3 CloudSunColourAtForBake(SkyAtmParams atm, CloudLayer layer, vec3 positionKm, vec3 toSun)
        {
            if (BakeClouds.w < 0.5f)
                return CloudSunColour.rgb;

            float sunZenithCos = clamp(dot(CloudLocalUp(positionKm), toSun), -1.0f, 1.0f);

            SkySunAtPoint sun = SkySunAtAltitude(atm.BottomRadiusKm, atm.TopRadiusKm,
                                                 CloudAltitudeKm(layer, positionKm), sunZenithCos,
                                                 vec2(textureSize(TransmittanceLutTexture, 0)));

            if (sun.PlanetShadow <= 0.0f)
                return vec3(0.0f, 0.0f, 0.0f);

            // textureLod and not texture, for the reason every other fetch in this file gives: a compute
            // shader has no derivatives, so the implicit level of detail is undefined.
            return CloudSunColour.rgb *
                   (sun.PlanetShadow * textureLod(TransmittanceLutTexture, sun.Uv, 0.0f).rgb);
        }

        CloudBakeResult MarchCloudsForBake(SkyPacked s, SkyAtmParams atm, vec3 dir, float jitter)
        {
            CloudBakeResult result;
            result.Luminance     = vec3(0.0f, 0.0f, 0.0f);
            result.Transmittance = 1.0f;

            CloudLayer       layer  = CloudUnpackLayer();
            CloudFieldParams params = CloudUnpackFieldParams();

            // WHERE THE CAPTURE STANDS IN THE CLOUD FIELD, and it is derived rather than sent.
            //
            // The modelling volume covers one square region of the wind-shifted frame, snapped to the
            // lump lattice around the camera; the march asks the volume about `position - wind`. Putting
            // the anchor at the region's own centre PLUS the wind therefore puts it back at the camera, to
            // within one snap step, without this pass having to be told where the camera is — and being
            // told would be the one thing that must not happen here, because the environment deliberately
            // does not rebake when the camera moves (the irradiance cube is a 65 536-sample cosine
            // convolution per texel: it integrates the arrangement of the field away and responds only to
            // the dome's mean).
            float regionSizeKm = 1.0f / max(CloudLevel[2].z, 1e-6f);
            vec2  anchorXz     = CloudLevel[2].xy + vec2(regionSizeKm * 0.5f) +
                             vec2(CloudWind.x, CloudWind.z);

            // The same planet-centred frame the screen march builds, at the same fixed altitude the sky
            // half of this bake is anchored at.
            vec3 originKm = vec3(anchorXz.x, layer.PlanetRadiusKm + kBakeAnchorAltitudeKm, anchorXz.y);

            vec2 segment = CloudLayerIntersect(layer, originKm, dir);

            // The authored limits, measured FROM THE LAYER ENTRY exactly as the screen march measures them
            // — Unreal's DistanceFromCloudLayerEntryPoint mode.
            segment.x = max(segment.x, CloudMarch.z);
            segment.y = min(segment.y, segment.x + max(CloudLayerGeometry.w, 0.0f));

            // A ray whose entry is past the cutoff is not traced, and one that never entered the shell —
            // every ray below the horizon, which is half of the cube — costs the two sphere tests above
            // and nothing else.
            if (segment.x > max(CloudPhase.w, 0.0f) || segment.y <= segment.x)
                return result;

            float length_km = segment.y - segment.x;
            float stepCount = CloudStepCount(length_km, CLOUD_MIN_STEPS, max(CloudMarch.x, CLOUD_MIN_STEPS),
                                             CLOUD_DISTANCE_TO_MAX_STEPS_KM);
            int   stepTotal = int(stepCount);

            float stepKm       = CloudFineStepKm(length_km, CLOUD_MIN_STEPS, max(CloudMarch.x, CLOUD_MIN_STEPS),
                                                 CLOUD_DISTANCE_TO_MAX_STEPS_KM);
            float coarseStepKm = CloudCoarseStepKm(stepKm);

            vec3  sunDir       = normalize(CloudSun.xyz);
            float phase        = CloudPhaseDualLobe(dot(dir, sunDir), CloudWind.w,
                                                    CloudPhase.x, CloudPhase.y);
            float extinction   = max(CloudMarch.w, 0.0f);
            // PER COLOUR, and read from the same slot the screen march reads it from — a capture lit by a
            // different albedo than the one the eye sees is the "second model of the clouds" this file's
            // own header refuses.
            vec3  albedo       = clamp(CloudAlbedo.rgb, vec3(0.0f), vec3(1.0f));
            float lightMarchKm = max(CloudSun.w, 0.0f);
            int   lightSamples = int(clamp(CloudSunColour.w, 1.0f, 64.0f));
            float stopT        = clamp(CloudMarch.y, 0.0f, 1.0f);

            CloudScatterSeries series;
            series.Octaves     = CloudMultiScatter.x;
            series.ScatterStep = CloudMultiScatter.y;
            series.ExtinctStep = CloudMultiScatter.z;
            series.PhaseStep   = CloudMultiScatter.w;

            vec3 ambientRadiance = CloudAmbient.rgb;
            if (CloudAmbient.w > 0.5f)
            {
                ambientRadiance = CloudAmbient.rgb *
                                  texelFetch(DistantSkyLightTexture, ivec2(SKY_DISTANT_LIGHT_SPHERE_TEXEL, 0), 0).rgb;
            }

            vec3  luminance     = vec3(0.0f, 0.0f, 0.0f);
            float transmittance = 1.0f;
            float t             = segment.x + jitter * stepKm;

            // The transmittance-weighted mean distance the aerial perspective is evaluated at — a cloud is
            // not at one distance, but the air in front of it has to be integrated over one.
            float aerialWeightedT = 0.0f;
            float aerialWeightSum = 0.0f;

            // The two-tier state, and both tiers judge by the SAME quantity — the un-eroded profile. See
            // the note in CloudRaymarch.shader: judging the tiers by different quantities makes the net
            // advance per cycle zero and the march stands still while burning its whole budget.
            bool fine     = false;
            int  emptyRun = 0;

            for (int i = 0; i < stepTotal; ++i)
            {
                if (t >= segment.y)
                    break;

                vec3  samplePos      = originKm + dir * t;
                float heightFraction = CloudHeightFraction(layer, samplePos);

                // The ALTITUDE above the layer's base, never the planet-relative height: the noise is
                // periodic and float32 resolves 0.4 m at 6363 km.
                vec3 fieldPos = vec3(samplePos.x, length(samplePos) - layer.BottomRadiusKm, samplePos.z);

                CloudFieldSample field = SampleCloudField(params, heightFraction, fieldPos);

                if (!fine)
                {
                    if (field.Profile > 0.0f)
                    {
                        fine     = true;
                        emptyRun = 0;
                        t        = max(segment.x, t - coarseStepKm);
                        continue;
                    }

                    t += coarseStepKm;
                    continue;
                }

                if (field.Profile <= 0.0f)
                {
                    ++emptyRun;
                    if (emptyRun >= CLOUD_EMPTY_FINE_SAMPLES_BEFORE_COARSE)
                    {
                        fine     = false;
                        emptyRun = 0;
                    }
                    t += stepKm;
                    continue;
                }

                emptyRun = 0;

                {
                    float density = CloudSampleDensity(params, field, fieldPos);

                    // The near-anchor fade. It fires for a layer whose base is inside the fade distance of
                    // the anchor, which is the same condition that makes it fire on screen for a camera
                    // inside the deck; carried so the artist's two distances are not dead in this pass.
                    if (CloudFadeParams.z > 0.0f)
                        density *= smoothstep(CloudFadeParams.w, CloudFadeParams.z, t);

                    if (density > 0.0f)
                    {
                        // Through the MEDIUM, exactly as the screen march does it — the capture the
                        // scene is lit by must not judge the same sky by a second field.
                        float sigmaT = density * extinction *
                                       CloudSampleExtinctionFactor(params, field, fieldPos);

                        float opticalDepth = CloudLightOpticalDepth(layer, params, samplePos, sunDir,
                                                                    lightMarchKm, lightSamples, extinction);

                        // HOW MUCH OF THE SKY THIS SAMPLE CAN SEE, by whichever of the two geometries this
                        // view is running. The gate is the screen march's own, so the baked dome carries
                        // the same self-occlusion the visible one does.
                        float ambientOcclusion = CloudAmbientOcclusion(field.Profile, CloudPhase.z);
                        if (BakeClouds.y > 0.5f)
                        {
                            vec3 skyWindPos = vec3(fieldPos.x - params.WindOffsetKm.x,
                                                   fieldPos.y - params.WindOffsetKm.y,
                                                   fieldPos.z - params.WindOffsetKm.z);

                            vec3 skyUvw = CloudSkyOcclusionUvw(params.ProceduralLevel[CLOUD_PROCEDURAL_CLIP_LEVELS - 1].xy,
                                                               params.ProceduralLevel[CLOUD_PROCEDURAL_CLIP_LEVELS - 1].z,
                                                               heightFraction, skyWindPos);

                            ambientOcclusion =
                                CloudSkyOcclusion(textureLod(CloudSkyOcclusionVolumeTexture, skyUvw, 0.0f).r,
                                                  CloudPhase.z);
                        }

                        // The sun THIS sample sees, on the screen march's own terms — see the note at
                        // CloudSunColourAtForBake for why the bake owes this and cannot take the block's
                        // colour verbatim.
                        // The medium's per-sample occlusion and albedo, on the screen march's own terms.
                        ambientOcclusion = CloudSampleOcclusion(params, field, fieldPos, ambientOcclusion);
                        vec3 sampleAlbedo = CloudSampleAlbedo(params, field, fieldPos, albedo);

                        luminance += transmittance *
                                     CloudMultiScatterStep(series,
                                                           CloudSunColourAtForBake(atm, layer, samplePos, sunDir),
                                                           ambientRadiance * ambientOcclusion, opticalDepth,
                                                           phase, sigmaT, sampleAlbedo, stepKm);

                        // Emission, attenuated by the ray's history only — see the note at the same line
                        // of the screen march. A layer that glows has to glow in the capture the scene is
                        // LIT by, or the light in the world would come from a sky nobody can see.
                        luminance += transmittance * CloudSampleEmissive(params, field, fieldPos) * stepKm;

                        aerialWeightedT += t * transmittance;
                        aerialWeightSum += transmittance;

                        transmittance *= CloudBeerTransmittance(sigmaT, stepKm);

                        if (transmittance < stopT)
                            break;
                    }
                }

                t += stepKm;
            }

            // AERIAL PERSPECTIVE, integrated rather than fetched. CloudAerial.z is the screen march's own
            // gate and is 1 only when the physical model published an aerial-perspective volume this frame,
            // so the artistic gradient takes this branch exactly as rarely as it takes the volume — never.
            if (CloudAerial.z > 0.5f && aerialWeightSum > 0.0f)
            {
                // Clamped to the volume's far extent so a cloud past it gets what the screen's last froxel
                // slice would have given it, rather than an integral the screen never performs.
                float meanDistanceKm =
                     min(CloudAerialMeanDistanceKm(aerialWeightedT, aerialWeightSum, CloudAerial.y),
                         max(CloudAerial.x, 0.0f));
                float startDepthKm = max(BakeClouds.z, 0.0f);

                SkyScatterResult air =
                     SkyApIntegrateSegment(atm, originKm, dir, UnpackSunDirection(s),
                                           UnpackAtmosphereLightIlluminance(s),
                                           startDepthKm, max(meanDistanceKm, startDepthKm),
                                           vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f));

                // The art-direction tint belongs INSIDE the integration and the volume applies it on the
                // stored value for the same reason (the integral is linear in it); the alpha lane the
                // screen reads is the MEAN of the three channels, so this takes the mean too.
                vec3  inScatter = air.Luminance * UnpackSkyAndAerialPerspectiveLuminanceFactor(s);
                float airT      = (air.Transmittance.r + air.Transmittance.g + air.Transmittance.b) / 3.0f;

                // The composition and the ramp are the screen march's, from the header both include —
                // only WHERE the air came from differs between the two passes.
                luminance = CloudApplyAerialPerspective(
                     luminance, transmittance, inScatter, airT,
                     CloudAerialAmount(meanDistanceKm, CloudFadeParams.x, CloudFadeParams.y));
            }

            result.Luminance     = luminance;
            result.Transmittance = clamp(transmittance, 0.0f, 1.0f);
            return result;
        }

        // The direction of face texel @p st (its centre, in [0,1]^2) of cube face @p face: PanoramaToCubemap's
        // face table, so a cube this pass writes is laid out exactly as every other cube the engine samples.
        vec3 CaptureFaceDirection(uint face, vec2 st)
        {
            vec2 uv = 2.0 * vec2(st.x, 1.0 - st.y) - vec2(1.0);
            vec3 d;
            if (face == 0u)      d = vec3(1.0, uv.y, -uv.x);
            else if (face == 1u) d = vec3(-1.0, uv.y, uv.x);
            else if (face == 2u) d = vec3(uv.x, 1.0, -uv.y);
            else if (face == 3u) d = vec3(uv.x, -1.0, uv.y);
            else if (face == 4u) d = vec3(uv.x, uv.y, 1.0);
            else                 d = vec3(-uv.x, uv.y, -1.0);
            return normalize(d);
        }

        LocalSize(8, 8, 1);
        void main()
        {
            ivec2 size  = imageSize(outputFaces);
            ivec2 coord = ivec2(gl_GlobalInvocationID.x, gl_GlobalInvocationID.y + BakeFace.y);
            uint  face  = BakeFace.x + gl_GlobalInvocationID.z;
            if (coord.x >= size.x || coord.y >= size.y || face >= 6u)
                return;

            vec3 dir = CaptureFaceDirection(face, (vec2(coord) + 0.5) / vec2(size));

            SkyPacked s;
            for (int i = 0; i < SKY_PACKED_VEC4_COUNT; ++i)
                s.v[i] = SkyPackedData[i];

            SkyAtmParams atm = MakeBakeAtmosphere(s);

            vec3 color;
            if (UnpackSkyModelIsPhysical(s))
                color = EvaluatePhysicalSkyForBake(s, atm, dir);
            else
                color = EvaluateSky(dir, UnpackSunDirection(s), UnpackSunIntensity(s),
                                    UnpackSunAngularRadius(s), UnpackSkyConfig(s));

            if (BakeClouds.x > 0.5f)
            {
                // Hashed on the TEXEL and with no frame index in it — see the header. Deterministic, so
                // two captures of unchanged settings produce the same faces byte for byte, which is what
                // makes the environment's own noise floor zero and a pixel diff of it mean something.
                uint  jitterHash = CloudHashCell(uint(coord.x), uint(coord.y), face, 0x51ED270Bu);
                float jitter     = float(jitterHash & 0xFFFFu) * (1.0f / 65536.0f);

                CloudBakeResult clouds = MarchCloudsForBake(s, atm, dir, jitter);

                // The premultiplied over-operator the frame's composite uses, and the same one the height
                // fog uses: the alpha lane is TRANSMITTANCE, not opacity.
                color = clouds.Luminance + color * clouds.Transmittance;
            }

            imageStore(outputFaces, ivec3(coord, int(face)), vec4(color, 1.0));
        }
    }
}
