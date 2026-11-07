// DesertAsset {"Kind":"Shader","Guid":"06ecc1e66d6549aa9861d2bd9e87da72","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S RADIOSITY TRACE ON THE ASYNC COMPUTE QUEUE (GPU-GI-CRITPATH; UE LumenRadiosity.usf
// LumenRadiosityDistanceFieldTracingCS, which runs with the rest of Lumen's scene lighting on async compute): the
// ray of every atlas texel of the pages relit this frame (SceneFrame::LightingPages), the same per-texel trace as
// the raster path (Common/SurfaceCacheRadiosityTraceTexel.glslh), one 8x8 group per page tile (z = the page), and
// the history blend the raster path's pipeline does (src alpha) done here: history + (new - history) x weight.
// Picked by Scalability GlobalIllumination.SurfaceCacheAsyncRadiosity (SurfaceCacheRenderer::
// AddIndirectLightingPass); 0 keeps the raster pass on the graphics queue for an A/B.
// The program is compiled per tracer (GlobalIllumination::TracerVariant); it names none.
Shader "SurfaceCacheRadiosityTraceAsync"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/SurfaceCache.glslh>
        ReadBuffer(7) SurfaceLightingPages
        {
            uint lightingPages[];
        };
        #include <Common/SurfaceCacheRadiosityTraceTexel.glslh>

        // The trace atlas: the history read, the blended sample written (the raster path's colour target).
        layout(binding = 24, r11f_g11f_b10f) restrict uniform image2D RadiosityTraceAtlas;

        void main()
        {
            const SurfaceCardPage page  = surfaceCardPages[lightingPages[gl_WorkGroupID.z]];
            const vec2            atlas = vec2( textureSize( SurfaceCacheOpacityTexture, 0 ) );
            const ivec4           rect  = RadiosityPageTexels( page.physicalAtlasUVRect, atlas );
            const ivec2           local = ivec2( gl_GlobalInvocationID.xy );
            if ( any( greaterThanEqual( local, rect.zw ) ) )
                return;
            const ivec2 atlasTexel = rect.xy + local;
            const vec4  traced     = RadiosityTraceTexel( atlasTexel, page.card.x, page.cardUVRect,
                                                          page.physicalAtlasUVRect, page.card.z );
            const vec3  history    = imageLoad( RadiosityTraceAtlas, atlasTexel ).rgb;
            imageStore( RadiosityTraceAtlas, atlasTexel, vec4( mix( history, traced.rgb, traced.a ), 0.0 ) );
        }
    }
}
