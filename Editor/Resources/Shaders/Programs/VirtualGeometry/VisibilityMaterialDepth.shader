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

        // VG-SW-RASTER: the virtual geometry's SW rows [VgRows.x, VgRows.x + VgRows.y) - row - VgRows.x is the main
        // cut's VisibleClusters row - and the tables that name the IndirectGroup resolving each (RasterBinning.shader
        // GroupOf's walk: cluster -> instance -> primitive -> lod-0 draw slot of the DAG submesh -> bin -> group).
        // VgRows.y = 0: no VG word this frame (the tables are then whatever VisibilityBuffer binds, never read).
        // Read as 32-bit words at the structs' offsets, so this program keeps no buffer-reference or 64-bit type:
        // VgVisibleCluster 16 B (Instance 0, Cluster 4), GpuSceneInstance 64 B (Primitive 48), VgCluster 64 B
        // (Dag 60), VgClusterDag 64 B (Submesh 44), GpuPrimitivePassDraws 16 B (FirstDrawSlot 0, SubmeshCount 4),
        // GpuDrawSlot 16 B (Bin 0), GpuDrawBin 32 B (Group 20) - Common/VirtualGeometry.glslh, Common/GpuScene.glslh.
        ReadBuffer(4) VgVisibleClusters
        {
        	uvec4 VgVisible[];
        };
        ReadBuffer(5) VgInstances
        {
        	uvec4 VgInstanceRows[]; // 4 per GpuSceneInstance
        };
        ReadBuffer(6) VgPrimitiveDraws
        {
        	uvec4 VgPrimitiveDraw[];
        };
        ReadBuffer(7) VgDrawSlots
        {
        	uvec4 VgDrawSlot[];
        };
        ReadBuffer(8) VgDrawBins
        {
        	uvec4 VgDrawBin[]; // 2 per GpuDrawBin
        };
        ReadBuffer(9) VgClusters
        {
        	uvec4 VgClusterRows[]; // 4 per VgCluster
        };
        ReadBuffer(10) VgDags
        {
        	uvec4 VgDagRows[]; // 4 per VgClusterDag
        };

        PushConstant PushConstants
        {
        	uvec4 VgRows;   // x first VG row, y VG rows (0: none), z primitive rows of the pass, w draw slot rows
        	uvec4 VgTables; // x bin rows, y group count of the pass
        };

        // The material depth of VG row @p row, 0 (no resolve's value) when the pass does not draw it.
        float VgMaterialDepth(const uint row)
        {
        	const uvec4 visible   = VgVisible[row - VgRows.x];
        	const uint  primitive = VgInstanceRows[4u * visible.x + 3u].x;
        	if (primitive >= VgRows.z)
        		return 0.0;
        	const uvec4 draws = VgPrimitiveDraw[primitive];
        	const uint  dag   = VgClusterRows[4u * visible.y + 3u].w;
        	const uint  sub   = VgDagRows[4u * dag + 2u].w;
        	if (sub >= draws.y)
        		return 0.0;
        	const uint slot = draws.x + sub; // lod 0, as the cut and the binning take it
        	if (slot >= VgRows.w)
        		return 0.0;
        	const uint bin = VgDrawSlot[slot].x;
        	if (bin >= VgTables.x)
        		return 0.0;
        	const uint group = VgDrawBin[2u * bin + 1u].y;
        	return group < VgTables.y ? DesertVisibilityVgMaterialDepth(group) : 0.0;
        }

        void main()
        {
        	const ivec2 pixel = ivec2(gl_FragCoord.xy);
        	if (uint(pixel.x) >= Words[0] || uint(pixel.y) >= Words[1] || (SectionCount == 0u && VgRows.y == 0u))
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
        	// A VG SW row resolves by its IndirectGroup's draw (VgMaterialDepth), never by a grass section.
        	if (VgRows.y != 0u && row >= VgRows.x && row - VgRows.x < VgRows.y)
        	{
        		const float vgDepth = VgMaterialDepth(row);
        		if (vgDepth == 0.0)
        			discard;
        		gl_FragDepth = vgDepth;
        		return;
        	}
        	if (SectionCount == 0u || row < SectionFirst[0])
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
