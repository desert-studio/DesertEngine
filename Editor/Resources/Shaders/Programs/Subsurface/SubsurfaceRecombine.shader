// DesertAsset {"Kind":"Shader","Guid":"c83a6e21f04b4d9fa1e7253b6d9c0a48","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SubsurfaceRecombine"
{
    // "Subsurface: Recombine" (SSS-PROFILE, CONTRACT §5.4), UE PostProcessSubsurface.usf SubsurfaceRecombinePS: adds
    // StoredBaseColor x lerp(diffuse, scattered, Tint) onto the scene colour, which the composite left holding the
    // scattered texels' specular + emissive only. Drawn with ONE/ONE blending over the scene target; the stage writes
    // the colour only, so the velocity, responsive-AA and thin-geometry attachments are masked (VulkanPipeline
    // CreateColorBlendState) and keep what the G-buffer wrote. A texel the composite did not mark is discarded.

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        // DESERT_DEFERRED_TILES (GPU-TILE-CLASSIFY, SubsurfaceRenderer's tiled permutation; UE Substrate's SSS
        // over the complex ClassificationTiles): drawn as the COMPLEX tiles of the view's list (Common/
        // DeferredTiles.glslh) through its complex draw record, six vertices per tile, whose firstVertex starts
        // the vertex index at the complex list. A scattered texel is not DefaultLit, so it is in a complex tile:
        // the texels this leaves out are the ones the fragment stage discards.
        #ifdef DESERT_DEFERRED_TILES
        #include <Common/DeferredTiles.glslh>
        // 4: past the fragment stage's 0..3; the shared sampler is numbered after it (SharedSamplerBinding.hpp).
        ReadBuffer(4) DeferredTilesUB
        {
        	uint deferredTiles[];
        };
        #endif

        Out(0) vec2 v_TexCoord;

        void main()
        {
        #ifdef DESERT_DEFERRED_TILES
        	const uvec2 extent = uvec2(deferredTiles[kDeferredTileExtent], deferredTiles[kDeferredTileExtent + 1u]);
        	const uint  vertex = uint(gl_VertexIndex);
        	const uint  tile   = deferredTiles[kDeferredTileHeaderWords + vertex / kDeferredTileVertices];
        	v_TexCoord         = DeferredTileCornerUV(tile, vertex % kDeferredTileVertices, extent);
        	gl_Position        = vec4(ScreenUVToNdc(v_TexCoord), 0.0, 1.0);
        #else
        	v_TexCoord  = FullscreenTriangleUV();
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        #endif
        }
    }

    Fragment
    {
        #include <Common/SubsurfaceProfileLayout.glslh>

        Uniform sampler PointClampSampler;
        Uniform(0) texture2D SubsurfaceDiffuseTexture;   // rgb = demodulated diffuse, a = profile row + 1 (0 = none)
        #define SubsurfaceDiffuseTexture sampler2D(SubsurfaceDiffuseTexture, PointClampSampler)
        Uniform(1) texture2D SubsurfaceScatteredTexture; // "Subsurface: Burley"'s result
        #define SubsurfaceScatteredTexture sampler2D(SubsurfaceScatteredTexture, PointClampSampler)
        Uniform(2) texture2D GBufferATexture;            // rgb = BaseColor (UE StoredBaseColor)
        #define GBufferATexture sampler2D(GBufferATexture, PointClampSampler)
        ReadBuffer(3) SubsurfaceProfileAtlasUB
        {
        	vec4 profileAtlas[];
        };

        In(0) vec2 v_TexCoord;
        Out(0) vec4 oColor;

        void main()
        {
        	const ivec2 pixel   = ivec2(gl_FragCoord.xy);
        	const vec4  diffuse = texelFetch(SubsurfaceDiffuseTexture, pixel, 0);
        	if (diffuse.a < 0.5)
        		discard;
        	const uint row       = (uint(diffuse.a + 0.5) - 1u) * SSSS_ATLAS_WIDTH;
        	const vec3 tint      = profileAtlas[row + SSSS_TINT].rgb;
        	const vec3 scattered = texelFetch(SubsurfaceScatteredTexture, pixel, 0).rgb;
        	const vec3 baseColor = texelFetch(GBufferATexture, pixel, 0).rgb;
        	oColor = vec4(baseColor * mix(diffuse.rgb, scattered, tint), 0.0);
        }
    }
}
