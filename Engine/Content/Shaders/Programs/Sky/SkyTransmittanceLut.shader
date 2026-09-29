// DesertAsset {"Kind":"Shader","Guid":"f024e347a1b65f84df26dbfa5f2747ae","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkyTransmittanceLut"
{
    Compute
    {
        // The transmittance LUT of the physical atmosphere (Hillaire 2020): for every (view height,
        // view zenith angle) pair, how much of each wavelength survives the trip from that point to the
        // top of the atmosphere. 256x64 RGBA16F, re-dispatched by SkyboxRenderer ONLY when the
        // atmosphere parameter block changes — the texel is a function of the medium alone, not of the
        // camera, the sun or time.
        //
        // Everything of substance lives in Common/SkyMedium.glslh, which is compiled as C++ by the
        // SkyMedium test suite: this stage is the UV -> (r, mu) mapping, one exp(-opticalDepth) march,
        // and an imageStore.

        #include <Common/Atmosphere.glslh>
        #include <Common/SkyMedium.glslh>

        layout(binding = 0, rgba16f) restrict writeonly uniform image2D u_TransmittanceLut;

        // The SAME sky parameter buffer every other sky consumer reads (Graphic::kSkyPayloadBinding);
        // the medium block is vec4s 7-12. The binding is bound explicitly from C++
        // (ComputePipeline::SetStorageBuffer), so this number and that constant must stay equal.
        ReadBuffer(1) SkyBuffer
        {
            vec4 u_SkyPacked[SKY_PACKED_VEC4_COUNT];
        };

        // The step budget is SKY_TRANSMITTANCE_SAMPLE_COUNT, declared in Common/SkyMedium.glslh next to
        // the march it drives: the CPU evaluation that reddens the directional light
        // (Graphic::SunTransmittanceAtGround) marches the same text and must march the same number of
        // steps, or the light and this LUT answer one question twice.

        LocalSize(8, 8, 1);
        void main()
        {
            ivec2 size  = imageSize(u_TransmittanceLut);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            SkyPacked s;
            for (int i = 0; i < SKY_PACKED_VEC4_COUNT; ++i)
                s.v[i] = u_SkyPacked[i];

            SkyAtmParams atm = SkyMakeAtmParams(UnpackMediumRayleigh(s), UnpackMediumMie(s),
                                                UnpackMediumMieAbsorption(s), UnpackMediumOzone(s),
                                                UnpackMediumGround(s), UnpackMediumTentPlanet(s));

            // TWO DIFFERENT THINGS GET CALLED "the texel-centre remap", and this line does one and not
            // the other. It does NOT apply SkyUnitToTexelUv (0.5/n + x*(n-1)/n), the UNIT-RANGE remap
            // that the multi-scatter and sky-view LUTs need, because Bruneton's distance
            // parameterisation already places the domain ends on the edge texels — applying it here
            // would compress the domain a second time.
            //
            // It DOES address the centre of its own texel, which is not optional: a compute shader
            // writing texel `coord` is read back by `texture()`, and hardware bilinear maps a uv to
            // texel space as uv*size - 0.5. So the value stored at `coord` is returned exactly when the
            // reader's uv is (coord + 0.5)/size, and this line is what makes the reader's
            // SkyTransmittanceLutUvFromParams the exact inverse of this write.
            //
            // The comment that stood here said "the RAW uv (no texel-centre remap)" one line above the
            // + 0.5f that is a texel-centre address, and the two readers (SkyViewLut,
            // SkyAerialPerspectiveLut) repeated the claim about this file. The CODE was right in all
            // three places; only the sentence was wrong. Nothing was ever mis-sampled — but the next
            // person to trust that sentence would have "fixed" this line and broken the pair.
            vec2 uv = (vec2(coord) + 0.5f) / vec2(size);

            SkyTransmittanceLutCoord c =
                 SkyTransmittanceLutParamsFromUv(atm.BottomRadiusKm, atm.TopRadiusKm, uv);

            vec3 transmittance =
                 SkyTransmittanceToTop(atm, c.ViewHeightKm, c.ViewZenithCos, SKY_TRANSMITTANCE_SAMPLE_COUNT);

            imageStore(u_TransmittanceLut, coord, vec4(transmittance, 1.0f));
        }
    }
}
