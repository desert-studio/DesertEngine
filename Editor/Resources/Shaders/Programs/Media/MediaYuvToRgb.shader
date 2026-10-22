// DesertAsset {"Kind":"Shader","Guid":"037e6bb5eeb84edaa55af065b15d5d23","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MediaYuvToRgb"
{
    Compute
    {
        // A decoded video frame's Y, U and V planes (Engine/Media/MediaTexture.cpp uploads them exactly as
        // dav1d hands them over: R8 for 8-bit, R16 for 10/12-bit with the samples in the low bits) become
        // one RGBA8 picture. The matrix (BT.601/709/2020) and the range (limited or full) come from the
        // bitstream, per frame. The output stays in the video's own transfer (gamma-encoded R'G'B'), the
        // same encoding every 8-bit UI texture carries.

        LocalSize(16, 16, 1);

        // Separate textures read by texelFetch only; the shared state at 4 exists because GLSL fetches through a
        // sampler (MediaTexture binds PointClamp there).
        Uniform(0) texture2D PlaneYTexture;
        Uniform(1) texture2D PlaneUTexture; // bound to Y when the frame is luma-only (Params.w == 1)
        Uniform(2) texture2D PlaneVTexture;
        Uniform(4) sampler PointClampSampler;
        #define PlaneYTexture sampler2D(PlaneYTexture, PointClampSampler)
        #define PlaneUTexture sampler2D(PlaneUTexture, PointClampSampler)
        #define PlaneVTexture sampler2D(PlaneVTexture, PointClampSampler)
        layout(binding = 3, rgba8) writeonly uniform image2D RgbFrameTexture;

        PushConstant PushConstants
        {
            vec4 Matrix; // x = Kr, y = Kb, z = sample scale (65535 / (2^bitDepth - 1), 1 for 8-bit), w = full range
            vec4 Params; // w = luma only (I400)
        };

        void main()
        {
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            ivec2 size  = imageSize(RgbFrameTexture);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            float scale = Matrix.z;
            float y     = texelFetch(PlaneYTexture, coord, 0).r * scale;
            float cb    = 0.5;
            float cr    = 0.5;
            if (Params.w < 0.5)
            {
                // Chroma planes may be subsampled (4:2:0, 4:2:2) or not (4:4:4): the plane's own size
                // relative to luma says which, so one shader serves all three layouts.
                ivec2 chromaSize  = textureSize(PlaneUTexture, 0);
                ivec2 chromaCoord = min(coord * chromaSize / textureSize(PlaneYTexture, 0), chromaSize - 1);
                cb = texelFetch(PlaneUTexture, chromaCoord, 0).r * scale;
                cr = texelFetch(PlaneVTexture, chromaCoord, 0).r * scale;
            }

            if (Matrix.w < 0.5)
            {
                // Limited ("studio") range: luma 16..235, chroma 16..240 in 8-bit code values.
                y  = (y - 16.0 / 255.0) * (255.0 / 219.0);
                cb = (cb - 128.0 / 255.0) * (255.0 / 224.0);
                cr = (cr - 128.0 / 255.0) * (255.0 / 224.0);
            }
            else
            {
                cb -= 128.0 / 255.0;
                cr -= 128.0 / 255.0;
            }

            float kr = Matrix.x;
            float kb = Matrix.y;
            float kg = 1.0 - kr - kb;
            float r  = y + 2.0 * (1.0 - kr) * cr;
            float b  = y + 2.0 * (1.0 - kb) * cb;
            float g  = (y - kr * r - kb * b) / kg;

            imageStore(RgbFrameTexture, coord, vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0));
        }
    }
}
