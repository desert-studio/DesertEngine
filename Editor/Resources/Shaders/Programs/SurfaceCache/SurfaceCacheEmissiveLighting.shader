// DesertAsset {"Kind":"Shader","Guid":"89f030de75af4f38be2b48ed0840e133","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S EMISSIVE LIGHTING (GI-TEMPORAL; contract Common/EmissiveCardLighting.glslh; UE MegaLights'
// rule for a stochastic light estimate: it reaches its reader only after temporal accumulation,
// MegaLightsDenoiserTemporal.usf): every page relit this frame (SceneFrame::LightingPages), ahead of the direct
// lighting, is drawn into the EmissiveLighting atlas layer, one fragment per atlas texel. A texel takes a Monte Carlo
// estimate of the scene's emissive triangles' light (Common/SurfaceCacheEmissiveLightingTexel.glslh: resampled picks
// over the candidates of the list's selection grid, EmissiveGridUB at 47, unclamped), blended with the layer's
// history by the pipeline (SrcAlpha / OneMinusSrcAlpha; src alpha = the estimate's weight: 1 with
// kEmissiveSamplesFirstPass picks on a page captured this frame or after a reset, 1/32 or 1/4 with
// kEmissiveSamplesPerTexel otherwise).
// The direct lighting reads the layer; the estimate is never a texel's value raw.
// The program is compiled per tracer (GlobalIllumination::TracerVariant); it names none.
Shader "SurfaceCacheEmissiveLighting"
{
    Vertex
    {
        #include <Common/SurfaceCache.glslh>
        #include <Common/ScreenUV.glslh>
        ReadBuffer(0) SurfaceCardPageBuffer
        {
            SurfaceCardPage surfaceCardPages[];
        };
        // Indices into SurfaceCardPageBuffer: the pages relit this frame (SceneFrame::LightingPages).
        ReadBuffer(1) SurfaceLightingPages
        {
            uint lightingPages[];
        };
        layout(location = 0) out flat uint v_CardIndex;
        layout(location = 1) out flat vec4 v_CardUVRect;
        layout(location = 2) out flat vec4 v_AtlasUVRect;
        layout(location = 3) out flat uint v_Captured;

        void main()
        {
            const SurfaceCardPage page    = surfaceCardPages[lightingPages[gl_InstanceIndex]];
            const vec2            corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                                       vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
            const vec2 uv = page.physicalAtlasUVRect.xy + corners[gl_VertexIndex] * page.physicalAtlasUVRect.zw;
            v_CardIndex   = page.card.x;
            v_CardUVRect  = page.cardUVRect;
            v_AtlasUVRect = page.physicalAtlasUVRect;
            v_Captured    = page.card.z; // 1: captured this frame (SurfaceCacheScene::Update)
            gl_Position   = vec4(ScreenUVToNdc(uv), 0.0, 1.0);
        }
    }
    Fragment
    {
        #include <Common/SurfaceCacheEmissiveLightingTexel.glslh>

        layout(location = 0) in flat uint v_CardIndex;
        layout(location = 1) in flat vec4 v_CardUVRect;
        layout(location = 2) in flat vec4 v_AtlasUVRect;
        layout(location = 3) in flat uint v_Captured;
        Out(0) vec4 oEmissive;

        void main()
        {
            oEmissive = EmissiveLightingTexel(ivec2(gl_FragCoord.xy), v_CardIndex, v_CardUVRect, v_AtlasUVRect,
                                              v_Captured);
        }
    }
}
