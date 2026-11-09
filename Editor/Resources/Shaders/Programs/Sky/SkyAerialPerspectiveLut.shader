// DesertAsset {"Kind":"Shader","Guid":"aec9b5461a0ab934bd3db04617facbca","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkyAerialPerspectiveLut"
{
    Compute
    {
        // The camera AERIAL-PERSPECTIVE volume of the physical atmosphere (Hillaire 2020, section 5.4):
        // 32x32x16 RGBA16F froxels covering THIS camera's frustum, refilled every frame.
        //
        // Per froxel: rgb = the light scattered into the eye by the air between the camera and that
        // froxel (premultiplied, linear HDR), a = the mean transmittance of the same stretch. That pair
        // is an over-operator, so a surface at distance d composites as
        //
        //     pixel = ap.rgb + pixel * ap.a
        //
        // and the atmosphere over the terrain becomes the same quantity as the atmosphere in the sky,
        // integrated over a shorter ray. That identity is the entire point of the pass — the terrain's
        // haze and the sky above the horizon can no longer disagree, because they are one march of one
        // medium through one pair of LUTs.
        //
        // ONE INVOCATION PER FROXEL, ONE QUADRATURE PER COLUMN (FPS-SKY-APLUT). Slice k's value is slice
        // k-1's plus one segment BY CONSTRUCTION: every invocation marches only ITS segment (tPrev..tNext,
        // the same sub-steps the column walk took) from (0, 1), and the column's 16 segments are then
        // chained in group-shared memory by the over-operator
        //
        //     L_k = L_k-1 + T_k-1 * Lseg_k,   T_k = T_k-1 * Tseg_k
        //
        // which is SkyApIntegrateSegment's own accumulation regrouped (it is linear in its luminance and
        // transmittance inputs). Re-marching from the camera per slice — UE's dispatch shape — gives
        // neighbouring slices slightly different quadrature, and at 16 slices over 96 km that difference
        // reads as a hard shell at each boundary that no amount of trilinear filtering hides; this shape
        // keeps the column's one quadrature and still spreads the march over 16384 invocations instead of
        // 1024 serial walks of up to 128 samples (the walk was latency-bound on 16 workgroups, so any async
        // compute sharing the GPU stretched it: 0.107 -> 0.193 ms when the upload batch forked the light
        // grid's async run beside it).
        //
        // The maths is Common/SkyScattering.glslh (SkyApIntegrateSegment and the slice mapping),
        // compiled as C++ by the SkyScattering tests from this same text. What is left here is the
        // froxel's ray, its segment, the chain, and a store.

        #include <Common/Atmosphere.glslh>
        #include <Common/SkyMedium.glslh>

        // rgba16f: in-scattered radiance is pre-tonemap HDR and transmittance is in [0,1]; half carries
        // three decimal digits, an order more than an over-operator needs.
        layout(binding = 0, rgba16f) restrict writeonly uniform image3D AerialPerspectiveLutTexture;

        // The SAME sky parameter buffer every other sky consumer reads (Graphic::kSkyPayloadBinding).
        ReadBuffer(1) SkyBuffer
        {
            vec4 SkyPackedData[SKY_PACKED_VEC4_COUNT];
        };

        // The cached pair, valid whenever this pass runs — SkyboxRenderer dispatches them first.
        // Bindings = Graphic::kSkyTransmittanceLutBinding / kSkyMultiScatterLutBinding.
        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform sampler GlobalFilterSampler;
        Uniform(2) texture2D TransmittanceLutTexture;
        #define TransmittanceLutTexture sampler2D(TransmittanceLutTexture, GlobalFilterSampler)
        Uniform(3) texture2D MultiScatterLutTexture;
        #define MultiScatterLutTexture sampler2D(MultiScatterLutTexture, GlobalFilterSampler)

        // Mirrored by Graphic::SkyAerialPerspectivePush.
        PushConstant SkyApPush
        {
            mat4 InverseViewProjection;
            vec4 CameraPositionWorld; // xyz = camera position in WORLD UNITS (centimetres)
            vec4 VolumeParams;   // x = volume depth (km), y = aerial perspective start depth (km)
        };

        // The transmittance LUT is addressed straight from its distance parameterisation: no
        // SkyUnitToTexelUv here, because Bruneton's mapping already puts the domain ends on the edge
        // texels. (Its write side DOES address texel centres — see SkyTransmittanceLut.shader — which
        // is what makes this read the exact inverse of that write; the two are different remaps and the
        // comment that used to stand here conflated them.) The multi-scatter LUT is parameterised in
        // unit range instead, so its read applies SkyUnitToTexelUv. Identical to the Sky-View fill's
        // pair, deliberately: the AP volume and the sky must sample the same texels.
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

        // THE CLOUD LAYER'S SHADOW ON THE AIR (UE VolumetricCloud "Shadow On Atmosphere"): the sun's single
        // scattering in every froxel segment times the cloud shadow map's transmittance at the sample — the
        // haze under a cloud is not lit by the sun the cloud blocks. The same bindings, block and receiver
        // text as the Sky-View fill (SkyboxRenderer binds both nodes alike): CloudShadowParams.y = 0 when no
        // cloud casts, and CloudShadowFactor is then 1 without a fetch — the volume is the unshadowed one.
        Uniform sampler LinearClampSampler;
        Uniform(4) texture2D CloudShadowMapTexture;
        #define CloudShadowMapTexture sampler2D(CloudShadowMapTexture, LinearClampSampler)
        ReadBuffer(5) SkyCloudShadowBuffer
        {
            mat4 CloudShadowWorldToMap;
            vec4 CloudShadowParams;
        };
        #include <Common/CloudShadowReceiver.glslh>

        // The march origin (planet-centred, WORLD-oriented here: SkyViewZenith * viewHeight) and whether the
        // light being integrated is the sun; main sets both. The sample goes back to world centimetres as an
        // offset from the camera, never through a planet-scale world coordinate.
        vec3  g_SkyMarchOriginKm      = vec3(0.0f, 0.0f, 0.0f);
        float g_SkyCloudShadowApplies = 0.0f;

        float SkyCloudShadowAt(vec3 positionKm)
        {
            if (g_SkyCloudShadowApplies < 0.5f)
                return 1.0f;
            return CloudShadowFactor(CameraPositionWorld.xyz +
                                     (positionKm - g_SkyMarchOriginKm) * SKY_WORLD_UNITS_PER_KM);
        }
        #define SKY_SCATTERING_SUN_VISIBILITY(position) SkyCloudShadowAt(position)

        #include <Common/SkyScattering.glslh>

        // The group: 4x4 froxel columns x the volume's 16 slices (Graphic::kAerialPerspectiveGroupColumns,
        // and the z extent IS kAerialPerspectiveDepth / SKY_AP_VOLUME_DEPTH - the C++ side asserts both).
        // Each column's segments meet in shared memory to be chained; one slot per invocation.
        const int AP_GROUP_COLUMNS = 4;
        const int AP_GROUP_SLICES  = 16;
        const int AP_GROUP_SIZE    = AP_GROUP_COLUMNS * AP_GROUP_COLUMNS * AP_GROUP_SLICES;
        shared vec3 s_SegmentLuminance[AP_GROUP_SIZE];
        shared vec3 s_SegmentTransmittance[AP_GROUP_SIZE];
        shared vec3 s_SegmentLight1Luminance[AP_GROUP_SIZE];

        LocalSize(4, 4, 16);
        void main()
        {
            ivec3 size  = imageSize(AerialPerspectiveLutTexture);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            int   slice = int(gl_LocalInvocationID.z);
            // No early return: every invocation reaches the barrier below. An invocation outside the
            // volume (none at 32x32x16; the guard keeps an odd extent honest) marches nothing and stores
            // nothing.
            const bool inside = coord.x < size.x && coord.y < size.y && slice < size.z;
            // The column's first slot: the slices of one column are AP_GROUP_COLUMNS^2 slots apart.
            const int column = int(gl_LocalInvocationID.y) * AP_GROUP_COLUMNS + int(gl_LocalInvocationID.x);
            const int slot   = slice * AP_GROUP_COLUMNS * AP_GROUP_COLUMNS + column;

            SkyPacked s;
            for (int i = 0; i < SKY_PACKED_VEC4_COUNT; ++i)
                s.v[i] = SkyPackedData[i];

            SkyAtmParams atm = SkyMakeAtmParams(UnpackMediumRayleigh(s), UnpackMediumMie(s),
                                                UnpackMediumMieAbsorption(s), UnpackMediumOzone(s),
                                                UnpackMediumGround(s), UnpackMediumTentPlanet(s));

            // The froxel column's ray. x and y are the TEXEL-CENTRE remap's unit domain, so froxel 0 is
            // exactly the screen's left/top edge and froxel N-1 exactly its right/bottom edge; the apply
            // pass reads back through SkyUnitToTexelUv, the exact inverse, and no pixel of the frame is
            // extrapolated from outside the volume.
            float unitX = float(coord.x) / max(float(size.x - 1), 1.0f);
            float unitY = float(coord.y) / max(float(size.y - 1), 1.0f);

            // The SAME reconstruction the apply pass uses, from the same inverse view-projection: the
            // froxel's ray and the pixel's ray have to be one direction, or the haze is sampled from the
            // neighbouring column at the frame's edges.
            vec2 ndc     = vec2(unitX * 2.0f - 1.0f, 1.0f - unitY * 2.0f);
            // Device depth 0 IS the far plane — the engine renders reversed-Z (Core/Projection.hpp).
            vec4 farH    = InverseViewProjection * vec4(ndc.x, ndc.y, 0.0f, 1.0f);
            vec3 farPos  = farH.xyz / max(farH.w, 1e-9f);
            vec3 rayDir  = normalize(farPos - CameraPositionWorld.xyz);

            // Planet-centred origin, built from the accurately-recovered view height rather than by
            // adding the planet radius to the camera's world Y — the SkyViewHeightKm argument, verbatim.
            // Clamped inside the shell: below the 10 m lift the ground intersection degenerates, above
            // the top the medium is empty and a space view is out of scope for a ground-level engine.
            vec3  cameraKm   = CameraPositionWorld.xyz / SKY_WORLD_UNITS_PER_KM;
            float viewHeight = clamp(SkyViewHeightKm(cameraKm, atm.BottomRadiusKm),
                                     atm.BottomRadiusKm + SKY_PLANET_RADIUS_OFFSET_KM,
                                     atm.TopRadiusKm - 0.01f);
            vec3 originKm      = SkyViewZenith(cameraKm, atm.BottomRadiusKm) * viewHeight;
            g_SkyMarchOriginKm = originKm;

            vec3 sunDir = UnpackSunDirection(s);

            // Outer-space sun illuminance: the authored sun colour times intensity — the same pair the
            // Sky-View fill integrates, so the two results are literally the same quantity.
            vec3 sunIlluminance = UnpackAtmosphereLightIlluminance(s);

            // The art-direction tint that belongs INSIDE every scattering integration (UE's
            // SkyAndAerialPerspectiveLuminanceFactor). Applied on the stored value because the integral
            // is linear in it — the same place and the same reasoning as the Sky-View fill.
            vec3 apTint = UnpackSkyAndAerialPerspectiveLuminanceFactor(s);

            float depthKm      = max(VolumeParams.x, 0.001f);
            float startDepthKm = max(VolumeParams.y, 0.0f);

            // Atmosphere light 1 (the moon): its own in-scattering through the SAME air (the transmittance is
            // the medium's, not a light's), accumulated beside the sun's and summed at the store.
            const bool light1      = UnpackLight1Present(s);
            vec3       light1Dir   = UnpackLight1Direction(s);
            vec3       light1Illum = UnpackLight1Illuminance(s);

            // THIS invocation's segment: from the previous slice's distance to its own (slice 0 spans
            // nothing, as the column walk's first step did), marched from (0, 1) so the chain below can
            // place it behind whatever the nearer slices put in front of it.
            const vec3 zero = vec3(0.0f, 0.0f, 0.0f);
            const vec3 one  = vec3(1.0f, 1.0f, 1.0f);
            SkyScatterResult segment;
            segment.Luminance     = zero;
            segment.Transmittance = one;
            vec3 light1Segment    = zero;
            if (inside)
            {
                float sliceDenom = max(float(size.z - 1), 1.0f);
                float tPrev      = SkyApDistanceFromSliceUnit(float(max(slice - 1, 0)) / sliceDenom, depthKm);
                float tNext      = SkyApDistanceFromSliceUnit(float(slice) / sliceDenom, depthKm);
                if (slice == 0)
                    tPrev = tNext;

                // Aerial Perspective Start Depth: everything nearer than it is simply not marched, so a
                // surface inside that distance keeps its own colour exactly. Both ends are lifted, so a
                // segment straddling the start contributes only its far part.
                g_SkyCloudShadowApplies = 1.0f; // the sun: the clouds cast along it
                segment = SkyApIntegrateSegment(atm, originKm, rayDir, sunDir, sunIlluminance,
                                                max(tPrev, startDepthKm), max(tNext, startDepthKm), zero, one);
                g_SkyCloudShadowApplies = 0.0f; // light 1: no map is cast along it
                if (light1)
                    light1Segment = SkyApIntegrateSegment(atm, originKm, rayDir, light1Dir, light1Illum,
                                                          max(tPrev, startDepthKm), max(tNext, startDepthKm),
                                                          zero, one).Luminance;
            }
            s_SegmentLuminance[slot]       = segment.Luminance;
            s_SegmentTransmittance[slot]   = segment.Transmittance;
            s_SegmentLight1Luminance[slot] = light1Segment;
            barrier();

            if (!inside)
                return;

            // THE CHAIN, nearest segment first - the order the column walk accumulated in. Light 1 rides the
            // sun's transmittance: the air in front of a segment dims both lights alike.
            vec3 luminance       = zero;
            vec3 transmittance   = one;
            vec3 light1Luminance = zero;
            for (int k = 0; k <= slice; ++k)
            {
                const int kSlot = k * AP_GROUP_COLUMNS * AP_GROUP_COLUMNS + column;
                luminance       = luminance + transmittance * s_SegmentLuminance[kSlot];
                light1Luminance = light1Luminance + transmittance * s_SegmentLight1Luminance[kSlot];
                transmittance   = transmittance * s_SegmentTransmittance[kSlot];
            }

            // Alpha is the MEAN of the three channels, as UE's volume stores it: the RGBA16F froxel
            // has one lane left and the over-operator downstream takes one scalar. What is lost is
            // the spectral reddening of the BACKGROUND (the in-scattered light keeps its own
            // colour), which is second-order next to the light the air adds.
            float meanTransmittance = (transmittance.r + transmittance.g + transmittance.b) / 3.0f;

            imageStore(AerialPerspectiveLutTexture, ivec3(coord.x, coord.y, slice),
                       vec4((light1 ? luminance + light1Luminance : luminance) * apTint, meanTransmittance));
        }
    }
}
