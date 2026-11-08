// DesertAsset {"Kind":"Shader","Guid":"edbbc1bc44dd4359ad6ac3c780139967","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S DIRECT LIGHTING (GI-SURFACE-CACHE-4; UE LumenSceneDirectLighting.cpp/.usf + the combine of
// LumenSceneLighting.usf): every page picked this frame (SurfaceCacheScene's update budget, every page captured
// this frame) is relit texel by texel. A texel's world position is its card texel placed back at its captured depth
// (the card box: the card plane, minus the depth layer times the box depth), its normal the captured octahedral
// normal. It receives the SAME light the deferred composite shades the frame with: the sun with its cascades and
// the cloud layer, every point and spot light of the view's light buffers with its local shadow views - through the
// composite's own receiver texts (Mesh/CascadedShadow.glslh, Common/CloudShadowReceiver.glslh,
// Mesh/LocalLightShadow.glslh), so a card cannot be lit by a different formula than the surface it holds.
//
// Two targets, both R11G11B10F atlas layers: DirectLighting = the diffuse radiance the texel leaves, albedo / PI x
// the front irradiance (sum of radiance x cos x shadow on +N) + subsurface / PI x the back irradiance (the same on
// -N, see below); FinalLighting = direct + indirect + emissive (UE CombineLumenSceneLighting, Diffuse_Lambert),
// indirect = the page's IndirectLighting layer as SurfaceCacheIndirectLighting.shader last wrote it; zero on a page
// captured this frame (its layer holds a stranger's texels until that pass runs).
//
// THE LIGHT THROUGH A LEAF (FOLIAGE-LOOK-5): a texel of a subsurface model (the Subsurface layer, the capture's
// DesertSubsurfaceColor - TwoSidedFoliage's SubsurfaceColor) also takes every light on its far side, -N, times its
// subsurface colour: the sun with the cascades' TransmissionShadow (the thickness of what stands between the leaf
// and the sun, as the deferred composite gives TwoSidedFoliage's transmission), the local lights with their shadow
// from the far side. Without it a ray that met a leaf's underside under a lit canopy read black: the radiance cache
// above a dense crown held only dark undersides and the crown went dark (FOLIAGE-LOOK-4).
// WHERE THIS DIFFERS FROM UE (for the owner): UE's card capture folds SubsurfaceColor into the card's diffuse
// albedo (LumenCardPixelShader.usf) and lights it from +N only, so a leaf's underside under the sun stays as dark as
// an opaque one in the cache - the very defect here. The cache keeps the colour apart and lights the far side; a
// card texel is view-independent, so the far side is Lambert (the deferred's forward-scatter lobe needs a viewer).
//
// One instance per page to relight: six vertices cover the page's rectangle of the atlas; the fragment works out
// its card texel from the page's card rectangle. A texel the capture saw no surface in (opacity 0) is unlit.
//
// OFF THE VIEW'S SHADOW MAPS (GI-SURFACE-CACHE-5, GI-CARD-LOOKUP; UE LumenSceneDirectLightingShadowMask.usf, the
// traced shadow ray of TraceDistanceFieldShadows / bLumenUseGlobalSDF or the hardware shadow trace): a texel no
// cascade covers (behind the camera, past the shadow distance) and a texel of a shadow-casting point/spot light the
// local shadow budget refused a view get their shadow from a visibility ray of the ACTIVE GI TRACER
// (GITraceVisibility, Common/GITracer.glslh: the global distance field march or the ray query - the program is
// compiled per tracer, GlobalIllumination::TracerVariant) instead of "fully lit".
// THE SUN PAST THE CASCADES UNDER "Shadows: Distance Field Sun" (DF-SHADOWS, ShadowFilter.z = 1; UE Lumen's
// offscreen distance-field shadowing): under the software tracer the sun's ray past the cascades is the screen
// mask's own - the tracer's mesh fields of the texel's froxel (hard, its near field), then the sun's CONE through
// the global field from the same start, steps and reach as the screen's far field
// (Common/DistanceFieldShadowGlobalTrace.glslh), so the GI bounce off a far slope is shadowed, penumbra included,
// as the slope on screen. The cascades stay where they hold the texel (exact there) and hand over to that trace
// across the SAME band as the screen (Mesh/CascadedShadow.glslh DistanceFieldShadowHandOver: the last cascade's
// border, UE r.Shadow.CSM.TransitionScale), so a card does not switch at a line the screen fades across. The moon
// slot takes the same cone along its own direction and disc (the method's trace is per direction, not per sun);
// the screen has no moon mask (one mask, the sun's - DistanceFieldShadowRenderer), there the moon past its
// cascades stays lit as before. Under the hardware tracer the ray query stays: exact against the TLAS, as UE's
// hardware-traced offscreen card shadows, switched hard at the last cascade (no mask on screen to match).
//
// THE LIGHTS OF A TEXEL (UE LumenSceneDirectLightingCulling): the world light grid's list for the texel's cell
// (Mesh/LightGrid.glslh LightGridWorldLightsAt - the radiance cache clipmap cells around the camera, built by
// "LightGridWorld" from the same point/spot buffers), not every light of the view.
//
// THE EMISSIVE SURFACES AS LIGHTS (GI-EMISSIVE-LIGHTS; NVIDIA RTXDI's emissive triangles in one light list, the
// path tracer's next event estimation): under either GI tracer a texel also receives the scene's emissive
// triangles (Common/EmissiveLights.glslh, the ONE list RayTracing::EmissiveLightsFrame the path tracer samples):
// kEmissiveSamplesPerTexel triangles picked by power, a point on each by area, the segment's radiance estimate
// over the solid-angle density, shadowed by the tracer's visibility ray. A lamp's glowing shade lights the wall
// beside it in the cache, so every GI ray that meets that wall carries the lamp's bounce - not only the rays that
// happen to meet the small shade itself (UE Lumen's noise of small emitters: its cards take the emissive only as
// the emitter's own radiance).
// The radiosity (SurfaceCacheRadiosityTrace.shader) leaves the emission of the cards its rays meet out under the
// same tracer (GI_HIT_EXCLUDES_EMISSION): the emitter -> texel path is counted here, once.
// WHERE THIS DIFFERS FROM UE (for the owner): UE has no emissive light sampling in Lumen; MegaLights' emissive
// support is the path tracer's. The software tracer has no list (it is built from the TLAS segment records) -
// GI-EMISSIVE-LIGHTS-SW.
Shader "SurfaceCacheDirectLighting"
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
            v_Captured    = page.card.z; // 1: captured this frame (SurfaceCacheScene::Update)
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
        #include <Common/SurfaceCacheDirectLightingTexel.glslh>

        layout(location = 0) in flat uint v_CardIndex;
        layout(location = 1) in flat vec4 v_CardUVRect;
        layout(location = 2) in flat vec4 v_AtlasUVRect;
        layout(location = 3) in flat uint v_Captured;
        Out(0) vec4 oDirect;
        Out(1) vec4 oFinal;

        void main()
        {
            DirectLightingTexel(ivec2(gl_FragCoord.xy), v_CardIndex, v_CardUVRect, v_AtlasUVRect, v_Captured,
                                oDirect, oFinal);
        }
    }
}
