// DesertAsset {"Kind":"Shader","Guid":"b1d04f3c5a7e4c0f9a6e2d8870c51f36","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterWaveMips"
{
    Compute
    {
        // One mip of the waves' array from the mip above it: the mean of every field, the squared slopes' too, so
        // the variance a mip lost is their mean less its mean slope's square (WaterSurfaceField.glslh).
        #include <Water/WaterFFT.glslh>

        layout(binding = 0, rgba16f) restrict readonly uniform image2DArray WaterWavesSource;
        layout(binding = 1, rgba16f) restrict writeonly uniform image2DArray WaterWavesTarget;

        LocalSize(8, 8, 1);

        void main()
        {
            const ivec3 id   = ivec3(gl_GlobalInvocationID);
            const ivec2 size = imageSize(WaterWavesTarget).xy;
            if (any(greaterThanEqual(id.xy, size)) || id.z >= WATER_WAVES_LAYERS)
                return;
            const ivec2 last = imageSize(WaterWavesSource).xy - 1;
            const ivec2 o    = id.xy * 2;
            vec4        sum  = vec4(0.0);
            for (int k = 0; k < 4; ++k)
                sum += imageLoad(WaterWavesSource, ivec3(min(o + ivec2(k & 1, k >> 1), last), id.z));
            imageStore(WaterWavesTarget, id, 0.25 * sum);
        }
    }
}
