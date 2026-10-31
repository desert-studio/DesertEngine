// DesertAsset {"Kind":"Shader","Guid":"39c6285ffa40443fa7b2b3f3f2d3e7c6","Versions":{"SHDR":1},"Dependencies":[]}
// "Grass: Visibility Depth" (Engine/Graphic/VirtualGeometry/VisibilityBuffer.cpp; UE 5 Nanite's EmitDepthTargets
// between the main and the post pass): the depth of every pixel phase 1's raster won, written into the G-buffer
// depth WITHOUT the material, so the furthest HZB built next (phase 2's occlusion test, the screen traces) sees the
// grass, and the material resolve runs once, after phase 2's raster (SceneRenderer::AddFrameGrassVisibilityResolve).
// A full-screen triangle; a pixel whose word is 0 (nothing drawn) is discarded, every other writes the word's
// reversed-Z depth (its high 32 bits) through gl_FragDepth, tested CloserOrEqual (the raster tested the same depth,
// so the word is never behind it). The buffer is read as 32-bit words (the layout VisibilityClear.shader writes:
// header = words 0..3, pixel p = words 4 + 2p (payload), 4 + 2p + 1 (depth)), so this program needs no 64-bit type.
Shader "VisibilityDepth"
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
        ReadBuffer(0) Visibility
        {
        	uint Words[];
        };

        void main()
        {
        	const ivec2 pixel = ivec2(gl_FragCoord.xy);
        	if (uint(pixel.x) >= Words[0] || uint(pixel.y) >= Words[1])
        		discard;
        	const uint depthBits = Words[4u + 2u * (uint(pixel.y) * Words[0] + uint(pixel.x)) + 1u];
        	if (depthBits == 0u)
        		discard;
        	gl_FragDepth = uintBitsToFloat(depthBits);
        }
    }
}
