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

        // THE FORWARD GROUND IS SHADED AS A FORWARD MESH IS (Mesh/Surface/Pass_Forward.glslh), through the same
        // headers, so the Forward and the Deferred path light one ground the same way: the sun from the scene's
        // DirectionLightsUB with the cascades' shadow, the sun's contact shadow and the cloud layer; the point and
        // spot lights of the pixel's froxel with their shadows; and the image-based ambient (the sky's irradiance
        // and prefiltered cubes through the sky look, the split-sum LUT, the reflection captures over the sky).
        // Every block below is written by name by SceneFrameBinding::ApplyTo (TerrainRenderer::PrepareFrame: the
        // Lights, Shadow, Environment and CloudShadow groups this layout declares); every map, the light grid and
        // the captures are the view's scene/view inputs, bound by name by the terrain pass (BindSceneViewInputs).
        // The slot numbers are this layout's own (8/9/10 are the terrain's instance rows and maps, so the
        // environment trio takes the deferred composite's 17/18/19); nothing binds by number.
        //
        // The sun: the one statement of it every lit program reads (Common/DirectionLightsUB.glslh).
        #include <Common/DirectionLightsUB.glslh>
        // The cascaded shadow's two bindings; Mesh/CascadedShadow.glslh reads them by these names.
        layout( binding = 5 ) uniform sampler2DArray u_ShadowCascades;
        layout( binding = 7 ) uniform ShadowUB
        {
            mat4 u_LightViewProj[4];
            vec4 u_ShadowParams;      // x = tan(sun source angle / 2), y = enabled (>0.5), z = debug mode, w = cascade count
            vec4 u_DebugParams;
            vec4 u_CascadeTexelWorld; // per-cascade world size of one shadow-map texel
            vec4 u_CascadeDepthRange; // per-cascade world length the [0,1] map depth spans
            vec4 u_ContactShadowView; // x = contact shadow ray steps (0 = off), y = tan(fovY / 2), z = dither frame
            vec4 u_SunContactShadow;  // the sun's contact shadow (Common/ContactShadow.glslh)
        };
        #include <Mesh/CascadedShadow.glslh>
        // The contact shadows' march (the Forward path has no opaque scene depth to march: ShadowUB says 0 steps).
        layout( binding = 27 ) uniform sampler2D u_SceneDepth;          // Core::kSceneDepthBinding
        layout( binding = 28 ) uniform usampler2D u_SceneShadingWord;   // Core::kSceneShadingWordBinding
        #include <ShadingModels/ShadingModelContract.glslh>
        #define CONTACT_SHADOW_DEPTH        u_SceneDepth
        #define CONTACT_SHADOW_VP           u.JitteredViewProjection
        #define CONTACT_SHADOW_SHADING_WORD u_SceneShadingWord
        #include <Common/ContactShadow.glslh>
        // The point and spot lights with their shadows (Mesh/ForwardLocalLights.glslh: the light buffers 4/6/16,
        // the grid 29/30, the local shadow atlas and views 31/32).
        #include <Mesh/ForwardLocalLights.glslh>

        // The environment: the sky's two cubes, the sky look (Common/SkyLook.glslh) and the split-sum LUT.
        layout( binding = 17 ) uniform samplerCube u_EnvSpecularTex;
        layout( binding = 18 ) uniform samplerCube u_EnvIrradianceTex;
        layout( binding = 19 ) uniform sampler2D u_BRDFLUTTexture;
        layout( binding = 22 ) uniform SkyLookUB
        {
            vec4 YawCosSin; // xy = (cos yaw, sin yaw) - Graphic::SkyLookGPU
            vec4 Gain;      // rgb = tint * intensity
        }
        skyLook;
        // The reflection captures (Common/ReflectionEnvironment.glslh), in set 1 as the deferred composite declares
        // them, so the ground's ambient specular is the captures around it over the sky on both paths.
        #define DESERT_REFLECTION_CAPTURES
        #include <Common/ReflectionCaptureGpu.glslh>
        layout( std430, set = 1, binding = 0 ) readonly buffer ReflectionCapturesUB
        {
            uvec4                u_ReflectionCaptureHeader; // x = capture count
            ReflectionCaptureGpu u_ReflectionCaptures[];
        };
        Uniform(1, 1) samplerCubeArray u_ReflectionCaptureCubes;
        // THE ambient, the same text the forward meshes and the deferred composite compile (DesertSampleAmbient).
        #include <Mesh/AmbientIBL.glslh>
        // THE ONE BRDF: every light and the ambient go through the shading model dispatch the forward meshes and the
        // deferred composite call (ShadingModels/ShadingModelContract.glslh), on the surface the Deferred path's
        // G-buffer stores for the same texel (TerrainGBuffer.shader: DefaultLit, the tinted albedo, metallic 0,
        // kTerrainRoughness, no material occlusion).
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

            // The sun, occluded by the cascades and its contact shadow, then by the cloud layer - the order
            // Pass_Forward and the deferred composite assemble it in. The ground always receives sun shadows.
            const vec3  sunDir = normalize( -directionLights.directionLights.Direction.xyz ); // toward the sun
            int         cascade;
            const float shadow = ShadowFactor( v_WorldPos, N, sunDir, cascade ) *
                                 ContactShadowFactor( v_WorldPos, N, sunDir, u_SunContactShadow, -1.0 ) *
                                 CloudShadowFactor( v_WorldPos );

            vec3 light = vec3( 0.0 );
            for ( uint d = 0u; d < lightsMetadata.DirectionLightCount; d++ )
                light += DesertEvaluateShadingModel(
                     SHADING_MODEL_INDEX_DEFAULT_LIT,
                     DesertSunLight( directionLights.directionLights.Direction.xyz,
                                     directionLights.directionLights.ColorIntensity.rgb *
                                          directionLights.directionLights.ColorIntensity.a,
                                     shadow ),
                     surface, payload );

            // The point and spot lights of the pixel's froxel, each with its shadow (Mesh/ForwardLocalLights.glslh).
            const ForwardLightList lights = ForwardLightListAt( gl_FragCoord.xy, v_WorldPos );
            for ( uint j = 0u; j < lights.Count; j++ )
                light += DesertEvaluateShadingModel( SHADING_MODEL_INDEX_DEFAULT_LIT,
                                                     ForwardLocalLightAt( lights, j, v_WorldPos, N, -1.0 ), surface,
                                                     payload );

            // The image-based ambient, unoccluded (the ground's G-buffer occlusion is 1) and with no indirect bounce
            // (the Forward path has no gather: the same zero Pass_Forward passes).
            const vec3 ambient = DesertEvaluateShadingModelAmbient(
                 SHADING_MODEL_INDEX_DEFAULT_LIT, DesertSampleAmbient( surface, vec3( 0.0 ), 1.0 ), surface, payload );

            o_Color   = vec4( light + ambient, 1.0 );
            oVelocity = TerrainVelocity();
        }
    }
}
