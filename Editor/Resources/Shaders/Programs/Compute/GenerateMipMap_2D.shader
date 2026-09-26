// DesertAsset {"Kind":"Shader","Guid":"64f9177362be2afe7482dbc79746037e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "GenerateMipMap_2D"
{
    Compute
    {
        LocalSize(32, 32, 1);

        Uniform(0) sampler2D u_InputTexture;
        Uniform(1) writeonly image2D u_OutputTexture;

        void main() {
            ivec2 globalCoord = ivec2(gl_GlobalInvocationID.xy);
            ivec2 outputSize = imageSize(u_OutputTexture);

            if (globalCoord.x >= outputSize.x || globalCoord.y >= outputSize.y) {
                return;
            }

            ivec2 srcCoord = globalCoord * 2;

            vec4 s00 = texelFetch(u_InputTexture, srcCoord + ivec2(0, 0), 0);
            vec4 s10 = texelFetch(u_InputTexture, srcCoord + ivec2(1, 0), 0);
            vec4 s01 = texelFetch(u_InputTexture, srcCoord + ivec2(0, 1), 0);
            vec4 s11 = texelFetch(u_InputTexture, srcCoord + ivec2(1, 1), 0);

            vec4 color = (s00 + s10 + s01 + s11) * 0.25;
            imageStore(u_OutputTexture, globalCoord, color);
        }
    }
}
