// DesertAsset {"Kind":"Shader","Guid":"c9cb48dc9a624e08bb45f735fdb68391","Versions":{"SHDR":1},"Dependencies":[]}
// THE PATH TRACER'S RESOLVE (System::PathTracer, "PathTracing: Resolve"): the view's accumulation (PathTracer.shader:
// rgb = the sum of every path since the last restart, a = how many were summed) divided by its count, written over
// the scene colour before post - the path-traced image replaces the deferred one. Ported from UE 5.8
// Engine/Shaders/Private/PathTracing/PathTracingCompositingPixelShader.usf (radiance = sum / count), adapted: one
// accumulation image instead of UE's radiance + variance pair (no denoiser runs behind it yet). Written pre-exposed
// (x ViewFrame::PreExposure, as UE's x View.PreExposure) like every other writer of the scene colour.
Shader "PathTracerResolve"
{
    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        void main()
        {
            gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }

    Fragment
    {
        Uniform(1) sampler PointClampSampler;
        Uniform(0) texture2D AccumulationTexture;

        PushConstant ResolvePush
        {
            float PreExposure; // ViewFrame::PreExposure: the scene colour is stored pre-exposed
        };

        Out(0) vec4 o_Color;

        void main()
        {
            // The accumulation is the scene colour's size (the path tracer runs at native scale), so the fragment's
            // own texel is the one its paths were summed into.
            const vec4 sum = texelFetch(sampler2D(AccumulationTexture, PointClampSampler), ivec2(gl_FragCoord.xy), 0);
            o_Color        = vec4(sum.rgb / max(sum.a, 1.0) * PreExposure, 1.0);
        }
    }
}
