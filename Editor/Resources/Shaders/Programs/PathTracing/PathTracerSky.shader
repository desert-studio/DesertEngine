// DesertAsset {"Kind":"Shader","Guid":"3668379a081a402989042e66739d2752","Versions":{"SHDR":1},"Dependencies":[]}
// THE PATH TRACER'S SKY LIGHT (Common/PathTracingSky.glslh): the environment as an equal-area square and the
// pyramid of its weights, rebuilt each frame the view path-traces (System::PathTracer::AddSkyPasses). Ported from
// UE 5.8 Engine/Shaders/Private/PathTracing/PathTracingSkylightPrepare.usf:1-36 (PathTracingSkylightPrepareCS),
// adapted: one environment cube (the scene's radiance cube, mip 0, through the sky look) instead of two blended
// captures, and the pdf mip chain as one buffer of levels reduced here ("Reduce") instead of a texture's mips.
Shader "PathTracerSky"
{
    Pass "Prepare"
    {
        Compute
        {
            LocalSize(8, 8, 1);

            #include <Common/GIFrame.glslh>
            #include <Common/SkyLook.glslh>
            #include <Common/PathTracingSky.glslh>

            Uniform sampler LinearClampSampler;
            Uniform(0) textureCube EnvironmentSpecularTexture;
            #define EnvironmentSpecularTexture samplerCube(EnvironmentSpecularTexture, LinearClampSampler)
            layout(binding = 1, rgba32f) writeonly uniform image2D SkyRadianceTexture;
            layout(std430, binding = 2) writeonly buffer PathTracingSkyPdfUB
            {
                float skyPdf[];
            };
            layout(std430, binding = 3) readonly buffer GIFrameUB
            {
                GIFrame giFrame;
            };

            PushConstant PushConstants
            {
                uvec4 SkyParams; // x = side S (a power of two)
            };

            void main()
            {
                const uint side = SkyParams.x;
                const uvec2 id  = gl_GlobalInvocationID.xy;
                if (id.x >= side || id.y >= side)
                    return;
                const vec2 uv        = (vec2(id) + 0.5) / float(side);
                const vec3 direction = PathTracingSkyDirectionOfUV(uv);
                const vec3 radiance  = max(ApplySkyGain(textureLod(EnvironmentSpecularTexture,
                                                                   SkyLookDirection(direction, giFrame.skyYawCosSin.xy),
                                                                   0.0).rgb,
                                                        giFrame.skyGain.rgb),
                                           vec3(0.0));
                // The texel's weight: its largest channel (UE: Prob = max(r, g, b)).
                const float weight = max(radiance.r, max(radiance.g, radiance.b));
                imageStore(SkyRadianceTexture, ivec2(id), vec4(radiance, weight));
                skyPdf[id.y * side + id.x] = weight;
            }
        }
    }

    Pass "Reduce"
    {
        Compute
        {
            LocalSize(8, 8, 1);

            #include <Common/PathTracingSky.glslh>

            layout(std430, binding = 0) buffer PathTracingSkyPdfUB
            {
                float skyPdf[];
            };

            PushConstant PushConstants
            {
                uvec4 SkyParams; // x = side S of level 0, y = the level this dispatch writes (>= 1)
            };

            // One texel of level y: the MEAN of its 2x2 children in level y - 1, so the 1x1 level is the mean
            // weight that PathTracingSkyEval / PathTracingSkySampleLight divide by.
            void main()
            {
                const uint side  = SkyParams.x;
                const uint level = SkyParams.y;
                const uint s     = side >> level;
                const uvec2 id   = gl_GlobalInvocationID.xy;
                if (id.x >= s || id.y >= s)
                    return;
                const uint below = PathTracingSkyPdfLevelOffset(side, level - 1u);
                const uint row   = s * 2u;
                const uint c     = below + id.y * 2u * row + id.x * 2u;
                skyPdf[PathTracingSkyPdfLevelOffset(side, level) + id.y * s + id.x] =
                    0.25 * (skyPdf[c] + skyPdf[c + 1u] + skyPdf[c + row] + skyPdf[c + row + 1u]);
            }
        }
    }
}
