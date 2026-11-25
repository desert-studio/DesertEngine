// DesertAsset {"Kind":"Shader","Guid":"9d4b1c7e35a84f0fa6c2d81b70e3954a","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterShoreUpload"
{
    Compute
    {
        // THE SHORE FIELD'S UPLOAD (WaterShoreUpload.cpp): one rectangle of the bake's packed texels into the map
        // the water samples (Water/WaterDepthField.glslh); the rest of the map stays as it is.
        Buffer(0) WaterShoreUploadParams
        {
            ivec4 ShoreRect; // the rectangle [x0, x1) x [z0, z1) as (x0, z0, x1, z1)
            ivec4 ShoreRows; // x = the rows' width (the buffer starts at row z0, texel 0)
        };
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
