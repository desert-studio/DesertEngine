// DesertAsset {"Kind":"Shader","Guid":"9d4b1c7e35a84f0fa6c2d81b70e3954a","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterShoreUpload"
{
    Compute
    {
        // THE SHORE FIELD'S UPLOAD (WaterShoreUpload.cpp): one rectangle of the bake's packed texels into the map
        // the water samples (Water/WaterDepthField.glslh); the rest of the map stays as it is.
        // ShoreRect, ShoreRows: Water/Private/WaterLayout.hpp ShoreUploadParams.
        #include <Generated/Water/WaterShoreUpload.glslh>
        Buffer(0) WaterShoreUploadParams { DESERT_ShoreUploadParams_MEMBERS };
        Buffer(1) WaterShoreUploadTexels
        {
            uvec2 ShoreTexels[]; // Water::PackShoreTexel
        };
        layout(binding = 2, rgba16f) restrict writeonly uniform image2D WaterShoreTarget;

        LocalSize(8, 8, 1);

        void main()
        {
            const ivec2 at = ShoreRect.xy + ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(at, ShoreRect.zw)))
                return;
            const uvec2 bits = ShoreTexels[(at.y - ShoreRect.y) * ShoreRows.x + at.x];
            imageStore(WaterShoreTarget, at, vec4(unpackHalf2x16(bits.x), unpackHalf2x16(bits.y)));
        }
    }
}
