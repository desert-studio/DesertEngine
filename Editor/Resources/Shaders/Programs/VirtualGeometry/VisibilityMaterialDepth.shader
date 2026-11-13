// DesertAsset {"Kind":"Shader","Guid":"868d5347dacd45dd89e72288ea12bedd","Versions":{"SHDR":1},"Dependencies":[]}
// "Grass: Visibility Material Depth" (Engine/Graphic/VirtualGeometry/VisibilityBuffer.cpp; UE 5 Nanite's
// EmitMaterialDepth, read by the material pass's depth EQUAL test): after phase 2's raster, a full-screen triangle
// into the view's material depth (DEPTH32F of its own, cleared to 0, depth test ALWAYS + write). A pixel whose word
// is drawn and not behind the G-buffer depth (reversed-Z, CloserOrEqual: the test the resolve used to run, so a
// phase-2 mesh in front keeps its texel) writes the material depth of the section owning the word's row - the
// largest first row of Sections not past it (GrassRenderer hands the rows out in ascending sections, one contiguous
// range each) - and every other pixel keeps 0, which no section's resolve tests EQUAL. The buffer is read as 32-bit
// words (VisibilityClear.shader's layout: header = words 0..3, pixel p = words 4 + 2p (payload), 4 + 2p + 1
// (depth)), so this program needs no 64-bit type.
Shader "VisibilityMaterialDepth"
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
        #include <Common/VisibilityTiles.glslh>

        Uniform(0) sampler PointClampSampler;
        Uniform(1) texture2D SceneDepthTexture;
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)

        ReadBuffer(2) Visibility
        {
        	uint Words[];
        };

        // Count, then the sections' first rows, ascending (VisibilityBuffer::SetSections).
        ReadBuffer(3) Sections
        {
        	uint SectionCount;
        	uint SectionFirst[];
        };

        void main()
        {
        	const ivec2 pixel = ivec2(gl_FragCoord.xy);
        	if (uint(pixel.x) >= Words[0] || uint(pixel.y) >= Words[1] || SectionCount == 0u)
        		discard;
        	const uint word      = 4u + 2u * (uint(pixel.y) * Words[0] + uint(pixel.x));
        	const uint payload   = Words[word];
        	const uint depthBits = Words[word + 1u];
        	// A word of 0 is nothing drawn (Common/VisibilityBuffer.glslh, VisibilityClassify.shader's test).
        	if (depthBits == 0u && payload == 0u)
        		discard;
        	if (uintBitsToFloat(depthBits) < texelFetch(SceneDepthTexture, pixel, 0).r)
        		discard; // a nearer surface holds the pixel
        	const uint row = payload >> 7u; // kVisTriangleBits
        	if (row < SectionFirst[0])
        		discard;
        	// The last section whose first row is not past `row`.
        	uint low  = 0u;
        	uint high = SectionCount - 1u;
        	while (low < high)
        	{
        		const uint middle = (low + high + 1u) >> 1u;
        		if (SectionFirst[middle] <= row)
        			low = middle;
        		else
        			high = middle - 1u;
        	}
        	gl_FragDepth = DesertVisibilityMaterialDepth(SectionFirst[low]);
        }
    }
}
