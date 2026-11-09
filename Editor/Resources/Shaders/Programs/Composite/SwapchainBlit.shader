// DesertAsset {"Kind":"Shader","Guid":"153c47916ea0c4d04785b281a7888c6a","Versions":{"SHDR":1},"Dependencies":[]}
// THE SDR DISPLAY ENCODE: the pass that writes the back buffer, so the one that quantises. The standalone runtime
// blits the scene's final image with it (RuntimeLayer RuntimePresent); the editor encodes its UI composite (the
// viewport image under the interface) with it (VulkanImGuiLayer DisplayEncode). No tonemap here (that already
// ran): the colour is copied, and dithered to one code of the back buffer's depth right before the write.
Shader "SwapchainBlit"
{
    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord  = FullscreenTriangleUV();
            gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }

    Fragment
    {
        In(0) vec2 v_TexCoord;

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(1) sampler LinearClampSampler;
        Uniform(0) texture2D FinalColorTexture;
        #define FinalColorTexture sampler2D(FinalColorTexture, LinearClampSampler)

        Out(0) vec4 o_Color;

        // The C++ side is Graphic::DisplayEncodePush (DisplayOutput.hpp).
        PushConstant PushConstants
        {
            float OutputQuantizationStep; // one back-buffer code, 1/(2^bits-1); 0 = nothing to quantise, no dither
        };

        // THE QUANTISATION DITHER (SDR-10BIT-DITHER). UE dithers in its tonemapper because the tonemapper writes
        // the LDR target; here FXAA / SMAA and the UI composite run between the tonemap and the back buffer and
        // would blend a dither applied earlier away (an edge, a translucent panel), so it is applied once, by the
        // last pass before the quantisation: this one. The noise is interleaved gradient noise (Jimenez 2014, what
        // UE PostProcessTonemap.usf dithers with) made triangular after Gjoel, "Banding in Games" (INSIDE, GDC
        // 2016): a TPDF dither of +-1 code makes the quantisation error's variance independent of the signal,
        // where a rectangular +-0.5 code still lets the error follow the gradient (noise modulation along bands).
        // A per-channel offset decorrelates R, G and B so the dither is not a grey grain.
        float DitherUniform(vec2 pixel, float channel)
        {
            const vec2 p = pixel + channel * vec2(47.0, 17.0);
            return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
        }

        // Uniform [0,1) -> triangular (-1,1) by the inverse CDF: the remapped noise keeps its spectrum.
        float TriangularFromUniform(float u)
        {
            const float n = u * 2.0 - 1.0;
            return sign(n) * (1.0 - sqrt(1.0 - abs(n)));
        }

        // +-1 code of triangular noise, narrowed within one code of black and white so the extremes stay exact
        // (a dither that lifts pure black to code 1 greys every shadow).
        vec3 DitherQuantization(vec3 encoded, vec2 pixel, float codeStep)
        {
            const vec3 noise = vec3(TriangularFromUniform(DitherUniform(pixel, 0.0)),
                                    TriangularFromUniform(DitherUniform(pixel, 1.0)),
                                    TriangularFromUniform(DitherUniform(pixel, 2.0)));
            const vec3 room  = clamp(min(encoded, 1.0 - encoded) / codeStep, 0.0, 1.0);
            return encoded + noise * room * codeStep;
        }

        void main()
        {
            vec4 color = texture(FinalColorTexture, v_TexCoord);
            // Keyed on gl_FragCoord (the back buffer's pixel), so the pattern is stationary on screen and never
            // crawls; the encoded value is clamped first, the range the UNORM write keeps anyway.
            if (OutputQuantizationStep > 0.0)
            {
                const vec3 encoded = clamp(color.rgb, 0.0, 1.0);
                color.rgb          = DitherQuantization(encoded, gl_FragCoord.xy, OutputQuantizationStep);
            }
            o_Color = color;
        }
    }
}
