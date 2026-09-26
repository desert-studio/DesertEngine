// DesertAsset {"Kind":"Shader","Guid":"095d74e1d55e9af6b957880a0aa6181d","Versions":{"SHDR":1},"Dependencies":[]}
Shader "JFA_Step"
{
    Fragment
    {
        In(0) vec2 v_TexCoord;

        Out(0) vec4 o_Result;

        Uniform(0, 0) sampler2D u_InputTexture;

        PushConstant PushConstants
        {
            int u_StepLength;
        };

        void main()
        {
            ivec2 texSize = textureSize(u_InputTexture, 0);
            vec2 pixelCoord = v_TexCoord * vec2(texSize);

            vec4 bestSeed = texture(u_InputTexture, v_TexCoord);
            float bestDist = 1e20;

            if (bestSeed.x >= 0.0)
            {
                bestDist = distance(bestSeed.xy, pixelCoord);
            }

            for (int y = -1; y <= 1; y++)
            {
                for (int x = -1; x <= 1; x++)
                {
                    if (x == 0 && y == 0)
                        continue;

                    vec2 offset = vec2(float(x), float(y)) * float(u_StepLength) / vec2(texSize);
                    vec4 neighborSeed = texture(u_InputTexture, v_TexCoord + offset);

                    if (neighborSeed.x >= 0.0)
                    {
                        float d = distance(neighborSeed.xy, pixelCoord);
                        if (d < bestDist)
                        {
                            bestDist = d;
                            bestSeed = neighborSeed;
                        }
                    }
                }
            }

            o_Result = bestSeed;
        }
    }

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/QuadTextureCoords.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];
            gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }
}
