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
        // ONE INVOCATION PER FROXEL COLUMN, not per froxel. The thread walks its ray outward carrying
        // (luminance, transmittance) from slice to slice, so slice k's value is slice k-1's plus one
        // segment BY CONSTRUCTION. Re-marching from the camera per slice — the obvious dispatch shape —
        // gives neighbouring slices slightly different quadrature, and at 16 slices over 96 km that
        // difference reads as a hard shell at each boundary that no amount of trilinear filtering hides.
        //
        // The maths is Common/SkyScattering.glslh (SkyApIntegrateSegment and the slice mapping),
        // compiled as C++ by the SkyScattering tests from this same text. What is left here is the
        // froxel's ray, the walk, and a store.

        #include <Common/Atmosphere.glslh>
        #include <Common/SkyMedium.glslh>

        // rgba16f: in-scattered radiance is pre-tonemap HDR and transmittance is in [0,1]; half carries
        // three decimal digits, an order more than an over-operator needs.
        layout(binding = 0, rgba16f) restrict writeonly uniform image3D u_AerialPerspectiveLut;

        // The SAME sky parameter buffer every other sky consumer reads (Graphic::kSkyPayloadBinding).
        ReadBuffer(1) SkyBuffer
        {
            vec4 u_SkyPacked[SKY_PACKED_VEC4_COUNT];
        };

        // The cached pair, valid whenever this pass runs — SkyboxRenderer dispatches them first.
        // Bindings = Graphic::kSkyTransmittanceLutBinding / kSkyMultiScatterLutBinding.
        Uniform(2) sampler2D u_TransmittanceLut;
        Uniform(3) sampler2D u_MultiScatterLut;

        // Mirrored by Graphic::SkyAerialPerspectivePush.
        PushConstant SkyApPush
        {
            mat4 u_InverseViewProjection;
            vec4 u_CameraPosWorld; // xyz = camera position in WORLD UNITS (centimetres)
            vec4 u_VolumeParams;   // x = volume depth (km), y = aerial perspective start depth (km)
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
            return texture(u_TransmittanceLut, uv).rgb;
        }

        vec3 SkySampleMultiScatterLut(SkyAtmParams atm, float radiusKm, float sunZenithCos)
        {
            vec2 unit = SkyMultiScatterUnitFromParams(atm.BottomRadiusKm, atm.TopRadiusKm, radiusKm,
                                                      sunZenithCos);
            vec2 uv   = vec2(SkyUnitToTexelUv(unit.x, 32.0f), SkyUnitToTexelUv(unit.y, 32.0f));
            return texture(u_MultiScatterLut, uv).rgb;
        }

        #define SKY_SCATTERING_SUN_TRANSMITTANCE(atm, radiusKm, sunZenithCos) SkySampleSunTransmittanceLut(atm, radiusKm, sunZenithCos)
        #define SKY_SCATTERING_MULTI_SCATTER(atm, radiusKm, sunZenithCos) SkySampleMultiScatterLut(atm, radiusKm, sunZenithCos)

        #include <Common/SkyScattering.glslh>

        LocalSize(8, 8, 1);
        void main()
        {
            ivec3 size  = imageSize(u_AerialPerspectiveLut);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            SkyPacked s;
            for (int i = 0; i < SKY_PACKED_VEC4_COUNT; ++i)
                s.v[i] = u_SkyPacked[i];

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
            vec4 farH    = u_InverseViewProjection * vec4(ndc.x, ndc.y, 0.0f, 1.0f);
            vec3 farPos  = farH.xyz / max(farH.w, 1e-9f);
            vec3 rayDir  = normalize(farPos - u_CameraPosWorld.xyz);

            // Planet-centred origin, built from the accurately-recovered view height rather than by
            // adding the planet radius to the camera's world Y — the SkyViewHeightKm argument, verbatim.
            // Clamped inside the shell: below the 10 m lift the ground intersection degenerates, above
            // the top the medium is empty and a space view is out of scope for a ground-level engine.
            vec3  cameraKm   = u_CameraPosWorld.xyz / SKY_WORLD_UNITS_PER_KM;
            float viewHeight = clamp(SkyViewHeightKm(cameraKm, atm.BottomRadiusKm),
                                     atm.BottomRadiusKm + SKY_PLANET_RADIUS_OFFSET_KM,
                                     atm.TopRadiusKm - 0.01f);
            vec3 originKm = SkyViewZenith(cameraKm, atm.BottomRadiusKm) * viewHeight;

            vec3 sunDir = UnpackSunDirection(s);

            // Outer-space sun illuminance: the authored sun colour times intensity — the same pair the
            // Sky-View fill integrates, so the two results are literally the same quantity.
            vec3 sunIlluminance = UnpackSkyConfig(s).sunColor * UnpackSunIntensity(s);

            // The art-direction tint that belongs INSIDE every scattering integration (UE's
            // SkyAndAerialPerspectiveLuminanceFactor). Applied on the stored value because the integral
            // is linear in it — the same place and the same reasoning as the Sky-View fill.
            vec3 apTint = UnpackSkyAndAerialPerspectiveLuminanceFactor(s);

            float depthKm      = max(u_VolumeParams.x, 0.001f);
            float startDepthKm = max(u_VolumeParams.y, 0.0f);

            vec3  luminance     = vec3(0.0f, 0.0f, 0.0f);
            vec3  transmittance = vec3(1.0f, 1.0f, 1.0f);
            float tPrev         = 0.0f;

            for (int slice = 0; slice < size.z; ++slice)
            {
                float unitZ = float(slice) / max(float(size.z - 1), 1.0f);
                float tNext = SkyApDistanceFromSliceUnit(unitZ, depthKm);

                // Aerial Perspective Start Depth: everything nearer than it is simply not marched, so a
                // surface inside that distance keeps its own colour exactly. Both ends are lifted, so a
                // segment straddling the start contributes only its far part.
                SkyScatterResult acc =
                     SkyApIntegrateSegment(atm, originKm, rayDir, sunDir, sunIlluminance,
                                           max(tPrev, startDepthKm), max(tNext, startDepthKm), luminance,
                                           transmittance);
                luminance     = acc.Luminance;
                transmittance = acc.Transmittance;
                tPrev         = tNext;

                // Alpha is the MEAN of the three channels, as UE's volume stores it: the RGBA16F froxel
                // has one lane left and the over-operator downstream takes one scalar. What is lost is
                // the spectral reddening of the BACKGROUND (the in-scattered light keeps its own
                // colour), which is second-order next to the light the air adds.
                float meanTransmittance =
                     (transmittance.r + transmittance.g + transmittance.b) / 3.0f;

                imageStore(u_AerialPerspectiveLut, ivec3(coord.x, coord.y, slice),
                           vec4(luminance * apTint, meanTransmittance));
            }
        }
    }
}
