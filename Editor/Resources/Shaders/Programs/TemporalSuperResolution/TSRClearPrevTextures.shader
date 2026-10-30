// DesertAsset {"Kind":"Shader","Guid":"3ed044e42b294976bb9f6d641eefd057","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRClearPrevTextures"
{
    Compute
    {
        // TSR.ClearPrevTextures. Ported from UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/
        // TSRClearPrevTextures.usf, adapted: the scatter target is a storage buffer (one uint per render pixel), not
        // an R32_UINT texture array; buffer atomics are core Vulkan and MoltenVK, image atomics are not guaranteed.
        // 0 is the reversed-Z far plane: no surface of this frame was here last frame.
        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        layout(std430, binding = 1) restrict writeonly buffer TSRPrevAtomicBuffer
        {
            uint PreviousAtomicData[];
        };

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(RenderSize))))
                return;
            PreviousAtomicData[pixel.y * int(RenderSize.x) + pixel.x] = 0u;
        }
    }
}
