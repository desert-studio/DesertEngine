// DesertAsset {"Kind":"Shader","Guid":"5c7221306c9643589755dfc28bebcaa7","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S INDIRECT LIGHTING (GI-SHADE-DARK; UE LumenRadiosity.usf LumenRadiosityIntegrateCS + the
// combine of LumenSceneLighting.usf): every page relit this frame (the same SceneFrame::LightingPages the direct
// lighting and the radiosity trace relit) integrates its texels' irradiance from the card's OWN radiosity probes
// (Common/SurfaceCacheRadiosity.glslh: the four nearest probes of the page, bilinear, each probe's traced hemisphere
// cosine-weighted about the texel's normal - SurfaceCacheRadiosityTrace.shader traced them this frame) and leaves
// IndirectLighting = albedo x that irradiance - plus, on a subsurface texel (the Subsurface layer), subsurface x the
// world radiance cache's irradiance at -N: the sky and bounce let through a leaf from its far side
// (FOLIAGE-LOOK-5; a probe's hemisphere holds only the front) - and recombines FinalLighting = direct + indirect +
// emissive. A traced hit then reads its bounce from the card in one fetch.
//
// As UE: the card's radiosity depends on no screen or world probe cache (GI-SHADE-DARK p1: the world cache was
// nearly empty at the surfaces, so shaded walls had no bounce at all and every ray reading them came back black).
// Runs after the radiosity trace of the frame; the trace's hits read the previous FinalLighting (one frame of lag,
// as UE's radiosity has).
Shader "SurfaceCacheIndirectLighting"
{
    Vertex
    {
        #include <Common/SurfaceCache.glslh>
        #include <Common/ScreenUV.glslh>
        ReadBuffer(0) SurfaceCardPageBuffer
        {
            SurfaceCardPage surfaceCardPages[];
        };
        ReadBuffer(1) SurfaceLightingPages
        {
            uint lightingPages[];
        };
        layout(location = 0) out flat uint v_CardIndex;
        layout(location = 1) out flat vec4 v_CardUVRect;
        layout(location = 2) out flat vec4 v_AtlasUVRect;

        void main()
        {
            const SurfaceCardPage page    = surfaceCardPages[lightingPages[gl_InstanceIndex]];
            const vec2            corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                                       vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
            const vec2 uv = page.physicalAtlasUVRect.xy + corners[gl_VertexIndex] * page.physicalAtlasUVRect.zw;
            v_CardIndex   = page.card.x;
            v_CardUVRect  = page.cardUVRect;
            v_AtlasUVRect = page.physicalAtlasUVRect;
            gl_Position   = vec4(ScreenUVToNdc(uv), 0.0, 1.0);
        }
    }
    Fragment
    {
        #include <Common/SurfaceCacheIndirectLightingTexel.glslh>

        layout(location = 0) in flat uint v_CardIndex;
        layout(location = 1) in flat vec4 v_CardUVRect;
        layout(location = 2) in flat vec4 v_AtlasUVRect;
        Out(0) vec4 oIndirect;
        Out(1) vec4 oFinal;

        void main()
        {
            IndirectLightingTexel( ivec2( gl_FragCoord.xy ), v_CardIndex, v_CardUVRect, v_AtlasUVRect, oIndirect,
                                   oFinal );
        }
    }
}
