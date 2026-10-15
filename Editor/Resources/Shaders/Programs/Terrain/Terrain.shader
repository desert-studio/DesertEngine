// DesertAsset {"Kind":"Shader","Guid":"9d6d6db70d7f0b397eac782bfb709efd","Versions":{"SHDR":1},"Dependencies":[]}
Shader "Terrain"
{
    // Data-driven material metadata. The ground's layers are the landscape's painted layer infos, not
    // parameters of this program: the material contributes the Tint every layer is multiplied by.

    Domain Terrain
    Role Terrain

    // Binding(1) is what makes these parameters a ROW of the shared `Materials[]` storage buffer, read
    // through `u_Material` in the fragment stage. The block used to be written out by hand below and was
    // a uniform block per material — one set of values for every terrain in the scene, which is the same
    // defect the graph materials had (Engine/Core/Formats/MaterialParamRow.hpp).
    Properties Binding(1)
    {
        Color       Tint ("Tint") = (1, 1, 1, 1)

        // The landscape tile's own maps (LandscapeECSSystem's R16 height / RGBA8 weight copies): material
        // parameters the TerrainRenderer writes per tile, never an artist (EngineSet). Declared by hand in
        // TerrainVertex.glslh / TerrainSurface.glslh; a Binding(1) row skips textures.
        Texture2D   u_Heightmap ("Heightmap", EngineSet)
        Texture2D   u_Weightmap ("Weightmap", EngineSet)
    }

    State
    {
        Topology Triangles
        Cull None
        ZTest Less
        ZWrite On
    }

    // ── The split every stage below repeats ─────────────────────────────────────────────────────────
    //
    // TerrainUB (binding 0) holds ONLY what every terrain of a frame shares: View, Projection and the
    // sun. Everything per-tile — its frame, sizes, layer modes — is a row of TerrainInstances[]
    // (binding 8), named per draw by the SAME push-constant index that names the material param row.
    //
    // WHY: the terrain pass records every draw of a frame before the GPU executes any of them, and a
    // material's uniform buffer has one copy per frame — so per-terrain fields kept in TerrainUB were
    // read as the LAST terrain's values by every terrain (two terrains drew as one). A push constant is
    // snapshotted at record time, so a row named by it cannot be clobbered by the next draw's setup —
    // the exact argument that already moved the material params to Materials[] (MaterialParamRow.hpp).
    // The C++ mirror of TerrainInstance lives in Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp,
    // and the ShaderCacheKey suite asserts both statements of the layout against the compiled SPIR-V.

    // The vertex stage is TerrainVertex.glslh, shared with TerrainGBuffer.shader and TerrainShadow.shader:
    // one statement of the grid, its LOD and its displacement, so the surface the cascades see is the
    // surface the camera sees. The vertex stage projects with the push-constant
    // matrix (m_PushConstants.Transform): the view's JitteredViewProjection here (raster geometry is jittered for TAA), the cascade's matrix in the shadow
    // pass — per draw, snapshotted at record, so one material serves every cascade.
    //
    // THIS PROGRAM IS THE FORWARD RENDER PATH'S. In Deferred the terrain writes the G-buffer instead
    // (TerrainGBuffer.shader) and this pass does not draw, so there is one lighting of the ground per path.

    Vertex
    {
        #include <Programs/Terrain/TerrainVertex.glslh>
    }

    Fragment
    {
        Out(0) vec4 o_Color;
        Out(1) vec2 oVelocity; // the view's velocity, slot 1 of the scene target (TerrainSurface.glslh)

        #include <Programs/Terrain/TerrainSurface.glslh>

        // THE CLOUD LAYER'S SHADOW ON THE WORLD. The terrain is the surface this feature is FOR — a deck
        // of cumulus drifting over open ground is the whole picture — and it is the surface that never
        // received it: the wrapper lived inside the deferred composite, and a terrain is drawn by neither
        // render path's mesh shaders. At the mesh shaders' slots 20/21: the point and spot lights below take
        // the lit programs' 4/6/7/16, and every material binds these by name (Graphic::CloudShadowBind).
        Uniform(20) sampler2D u_CloudShadowMap;
        Uniform(21) CloudShadowUB
        {
        	mat4 u_CloudShadowWorldToMap;
        	// x = the kilometres the map's clip z spans, y = 1 when the map is real and must be read,
        	// z = the UV width of the border fade, w = the artist's shadow strength.
        	vec4 u_CloudShadowParams;
        };

        // THE receiver, shared verbatim with the deferred composite and the mesh shaders.
        #include <Common/CloudShadowReceiver.glslh>
        // FORWARD ONLY: in the Deferred path the composite applies it to the terrain's G-buffer texels.

        // THE POINT AND SPOT LIGHTS, with their shadows, through the statement the forward meshes and the glass
        // shade them with (Mesh/ForwardLocalLights.glslh): in the Deferred path the composite lights the
        // terrain's G-buffer texels with them, so the Forward path's ground must not be the one surface they miss.
        // The lights' buffers and the cascades' ShadowUB (whose contact-shadow members the march reads) are
        // written by name by SceneFrameBinding (TerrainRenderer::PrepareFrame); the scene depth and shading word
        // the march reads, the light grid and the local shadow atlas are the view's scene-read inputs at their
        // reserved slots (Core::kSceneDepthBinding .., ShaderGraphBindings.hpp), bound by the terrain pass
        // (BindSceneViewInputs). The Forward path has no opaque scene depth to march (ShadowUB says 0 steps).
        layout( binding = 7 ) uniform ShadowUB
        {
            mat4 u_LightViewProj[4];
            vec4 u_ShadowParams;
            vec4 u_DebugParams;
            vec4 u_CascadeTexelWorld;
            vec4 u_CascadeDepthRange;
            vec4 u_ContactShadowView; // x = contact shadow ray steps (0 = off), y = tan(fovY / 2), z = dither frame
            vec4 u_SunContactShadow;
        };
        layout( binding = 27 ) uniform sampler2D u_SceneDepth;          // Core::kSceneDepthBinding
        layout( binding = 28 ) uniform usampler2D u_SceneShadingWord;   // Core::kSceneShadingWordBinding
        #include <ShadingModels/ShadingModelContract.glslh>
        #define CONTACT_SHADOW_DEPTH        u_SceneDepth
        #define CONTACT_SHADOW_VP           u.JitteredViewProjection
        #define CONTACT_SHADOW_SHADING_WORD u_SceneShadingWord
        #include <Common/ContactShadow.glslh>
        #include <Mesh/ForwardLocalLights.glslh>
        // THE ONE BRDF: every light the forward ground receives (the sun and each point/spot light) goes through
        // the shading model dispatch the forward meshes and the deferred composite call
        // (ShadingModels/ShadingModelContract.glslh), on the surface the Deferred path's G-buffer stores for the
        // same texel (TerrainGBuffer.shader: DefaultLit, the tinted albedo, metallic 0, kTerrainRoughness). So
        // one light gives the forward ground, the deferred ground and a forward mesh of that material the same
        // radiance — diffuse albedo / PI and the GGX lobe.
        #include <Generated/ShadingModels.glslh>

        void main()
        {
            TerrainSurface s = EvaluateTerrainSurface();
            vec3 N           = s.N;

            // The eye from the view matrix (rigid: its inverse rotation is its transpose).
            const vec3          eye     = -transpose( mat3( u.View ) ) * u.View[3].xyz;
            const DesertSurface surface = DesertMakeSurface( v_WorldPos, N, eye - v_WorldPos,
                                                             s.Albedo * u_Material.Tint.rgb, 0.0, kTerrainRoughness );
            const DesertPayload payload = DesertPayload( 0.0, 0.0, 0.0, 0.0 );

            // Soft sky/ground ambient (hemisphere): brighter from above, cooler/darker from below.
            vec3  skyCol  = vec3( 0.45, 0.52, 0.62 );
            vec3  gndCol  = vec3( 0.20, 0.18, 0.14 );
            vec3  ambient = mix( gndCol, skyCol, N.y * 0.5 + 0.5 ) * 0.5;

            // The SCENE directional light (TerrainUB SunDir / SunColor: intensity 0 when the scene has none).
            // The cloud layer attenuates the SUN and nothing else: the hemisphere ambient above is the
            // whole sky dome, which a deck occludes with a different geometry than a direction. Same
            // split the deferred composite and the mesh shaders make.
            vec3 light = DesertEvaluateShadingModel(
                 SHADING_MODEL_INDEX_DEFAULT_LIT,
                 DesertSunLight( u.SunDir.xyz, u.SunColor.rgb * u.SunColor.a, CloudShadowFactor( v_WorldPos ) ),
                 surface, payload );

            // The point and spot lights of the pixel's froxel, each with its shadow (Mesh/ForwardLocalLights.glslh).
            const ForwardLightList lights = ForwardLightListAt( gl_FragCoord.xy, v_WorldPos );
            for ( uint j = 0u; j < lights.Count; j++ )
                light += DesertEvaluateShadingModel( SHADING_MODEL_INDEX_DEFAULT_LIT,
                                                     ForwardLocalLightAt( lights, j, v_WorldPos, N, -1.0 ), surface,
                                                     payload );

            o_Color   = vec4( surface.BaseColor * ambient + light, 1.0 );
            oVelocity = TerrainVelocity();
        }
    }
}
