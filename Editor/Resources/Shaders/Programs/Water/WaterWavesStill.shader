// DesertAsset {"Kind":"Shader","Guid":"7c2e91d4a5f04b68b3e0d8172c6f5a94","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterWavesStill"
{
    Compute
    {
        // The waves' array of a frame with no FFT ocean: rest (Water/WaterSurfaceField.glslh reads it nowhere, and
        // a bound slot holds a defined image).
        #include <Water/WaterFFT.glslh>

        layout(binding = 0, rgba16f) restrict writeonly uniform image2DArray WaterWavesTarget;

        LocalSize(1, 1, 1);

        void main()
        {
            imageStore(WaterWavesTarget, ivec3(gl_GlobalInvocationID), vec4(0.0));
        }
    }
}
