// DesertAsset {"Kind":"Shader","Guid":"ec2cf83ab7234b04a0239d55a04ed53a","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S RADIOSITY TRACE (GI-SHADE-DARK; UE LumenRadiosity.usf LumenRadiosityDistanceFieldTracingCS /
// the hardware RayGen, with PlaceProbes folded in): every page relit this frame (SceneFrame::LightingPages) is drawn
// into the radiosity trace atlas, one fragment per atlas texel. A texel is one ray of its tile's probe
// (Common/SurfaceCacheRadiosity.glslh): the probe stands on the tile's first opaque texel, the ray leaves it in the
// texel's stratum of the probe normal's hemisphere (jittered per frame) through the GI ray contract - the frame's
// tracer, a hit read from the cards' FinalLighting (the previous frame's, one bounce more per frame as UE's
// radiosity), a miss the sky (GIMissRadiance; UE: the skylight leaking into a miss). The result is blended with the
// texel's history by the pipeline (src alpha = the new sample's weight; 1 on a page captured this frame or after a
// full relight, UE's temporal accumulation otherwise).
// The program is compiled per tracer (GlobalIllumination::TracerVariant); it names none.
Shader "SurfaceCacheRadiosityTrace"
{
    Vertex
    {
        #include <Common/SurfaceCache.glslh>
        #include <Common/ScreenUV.glslh>
        ReadBuffer(1) SurfaceCardPageBuffer
        {
            SurfaceCardPage surfaceCardPages[];
        };
        ReadBuffer(7) SurfaceLightingPages
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
            v_Captured    = page.card.z;
            gl_Position   = vec4(ScreenUVToNdc(uv), 0.0, 1.0);
        }
    }
    Fragment
    {
        #include <Common/SurfaceCacheRadiosityTraceTexel.glslh>

        layout(location = 0) in flat uint v_CardIndex;
        layout(location = 1) in flat vec4 v_CardUVRect;
        layout(location = 2) in flat vec4 v_AtlasUVRect;
        layout(location = 3) in flat uint v_Captured;
        Out(0) vec4 oRadiance;

        void main()
        {
            oRadiance = RadiosityTraceTexel( ivec2( gl_FragCoord.xy ), v_CardIndex, v_CardUVRect,
                                             v_AtlasUVRect, v_Captured );
        }
    }
}
