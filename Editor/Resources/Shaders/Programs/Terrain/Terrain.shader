Shader "Terrain"
{
    // Data-driven material metadata (consumed by the pipeline cache + generic material in later phases).
    // Splat layers — drag textures onto these in Details; unassigned = white fallback (shows the base tint).

    Domain Terrain

    // Binding(1) is what makes these parameters a ROW of the shared `Materials[]` storage buffer, read
    // through `u_Material` in the fragment stage. The block used to be written out by hand below and was
    // a uniform block per material — one set of values for every terrain in the scene, which is the same
    // defect the graph materials had (Engine/Core/Formats/MaterialParamRow.hpp).
    Properties Binding(1)
    {
        Color       Tint ("Tint") = (1, 1, 1, 1)
        Float       DetailTiling ("Texture Tiling (m)", Range(0.25,64)) = 4
        Texture2D   u_GrassTex ("Grass Texture")
        Texture2D   u_RockTex ("Rock Texture")
        Texture2D   u_SnowTex ("Snow Texture")
    }

    State
    {
        Topology Patches 4
        Cull None
        ZTest Less
        ZWrite On
    }

    // ── The split every stage below repeats ─────────────────────────────────────────────────────────
    //
    // TerrainUB (binding 0) holds ONLY what every terrain of a frame shares: View, Projection and the
    // sun. Everything per-terrain — Model, sizes, seed, layer modes — is a row of TerrainInstances[]
    // (binding 8), named per draw by the SAME push-constant index that names the material param row.
    //
    // WHY: the terrain pass records every draw of a frame before the GPU executes any of them, and a
    // material's uniform buffer has one copy per frame — so per-terrain fields kept in TerrainUB were
    // read as the LAST terrain's values by every terrain (two terrains drew as one). A push constant is
    // snapshotted at record time, so a row named by it cannot be clobbered by the next draw's setup —
    // the exact argument that already moved the material params to Materials[] (MaterialParamRow.hpp).
    // The C++ mirror of TerrainInstance lives in Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp,
    // and the ShaderCacheKey suite asserts both statements of the layout against the compiled SPIR-V.

    // The stage bodies are the three .glslh files beside this one, shared with TerrainGBuffer.shader and
    // TerrainShadow.shader: one statement of the patch, its tessellation and its displacement, so the
    // surface the cascades see is the surface the camera sees. TessEval projects with the push-constant
    // matrix (m_PushConstants.Transform): camera Projection * View here, the cascade's matrix in the shadow
    // pass — per draw, snapshotted at record, so one material serves every cascade.
    //
    // THIS PROGRAM IS THE FORWARD RENDER PATH'S. In Deferred the terrain writes the G-buffer instead
    // (TerrainGBuffer.shader) and this pass does not draw, so there is one lighting of the ground per path.

    Vertex
    {
        #include <Programs/Terrain/TerrainVertex.glslh>
    }

    TessControl
    {
        #include <Programs/Terrain/TerrainTessControl.glslh>
    }

    TessEval
    {
        #include <Programs/Terrain/TerrainTessEval.glslh>
    }

    Fragment
    {
        Out(0) vec4 o_Color;

        #include <Programs/Terrain/TerrainSurface.glslh>

        // THE CLOUD LAYER'S SHADOW ON THE WORLD. The terrain is the surface this feature is FOR — a deck
        // of cumulus drifting over open ground is the whole picture — and it is the surface that never
        // received it: the wrapper lived inside the deferred composite, and a terrain is drawn by neither
        // render path's mesh shaders. Slots 6/7 are the first free ones in this shader's own small layout;
        // they need not (and do not) match the mesh shaders' 20/21, because only the NAMES are shared and
        // every material binds by name (Graphic::CloudShadowBind).
        Uniform(6) sampler2D u_CloudShadowMap;
        Uniform(7) CloudShadowUB
        {
        	mat4 u_CloudShadowWorldToMap;
        	// x = the kilometres the map's clip z spans, y = 1 when the map is real and must be read,
        	// z = the UV width of the border fade, w = the artist's shadow strength.
        	vec4 u_CloudShadowParams;
        };

        // THE receiver, shared verbatim with the deferred composite and the mesh shaders.
        #include <Common/CloudShadowReceiver.glslh>
        // FORWARD ONLY: in the Deferred path the composite applies it to the terrain's G-buffer texels.

        void main()
        {
            TerrainSurface s = EvaluateTerrainSurface();
            vec3 N           = s.N;
            vec3 albedo      = s.Albedo;
            float wSnow      = s.Snow;

            // PBR-ish lighting using the SCENE directional light (dir/color/intensity). Camera position recovered
            // from the inverse view matrix for the specular view vector.
            vec3 camPos = inverse( u.View )[3].xyz;
            vec3 V      = normalize( camPos - v_WorldPos );
            vec3 L      = normalize( -u.SunDir.xyz );          // to-light (engine stores travel direction)
            vec3 sun    = u.SunColor.rgb * max( u.SunColor.a, 0.0001 );
            vec3 H      = normalize( L + V );

            float ndl = max( dot( N, L ), 0.0 );

            // Soft sky/ground ambient (hemisphere): brighter from above, cooler/darker from below.
            vec3  skyCol  = vec3( 0.45, 0.52, 0.62 );
            vec3  gndCol  = vec3( 0.20, 0.18, 0.14 );
            vec3  ambient = mix( gndCol, skyCol, N.y * 0.5 + 0.5 ) * 0.5;

            // Per-layer roughness: rock/grass matte, snow has a subtle sheen.
            float gloss = mix( 0.0, 0.5, clamp( wSnow, 0.0, 1.0 ) );
            float spec  = pow( max( dot( N, H ), 0.0 ), mix( 16.0, 90.0, gloss ) ) * gloss;

            // The cloud layer attenuates the SUN and nothing else: the hemisphere ambient above is the
            // whole sky dome, which a deck occludes with a different geometry than a direction. Same
            // split the deferred composite and the mesh shaders make.
            float cloudShadow = CloudShadowFactor( v_WorldPos );

            vec3 lit = albedo * ( ambient + sun * ndl * cloudShadow ) + sun * spec * ndl * cloudShadow;

            o_Color = vec4( lit * u_Material.Tint.rgb, 1.0 );
        }
    }
}
