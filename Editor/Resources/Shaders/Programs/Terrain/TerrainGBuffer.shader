// DesertAsset {"Kind":"Shader","Guid":"f5f5ad4fa89273327efd0e6c4b0ba655","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TerrainGBuffer"
{
    // The terrain in the DEFERRED render path: it writes the G-buffer like every other opaque surface and
    // is lit — sun, cascaded shadows, cloud shadow, sky, SSAO, GI — by the one deferred composite. Its own
    // forward lighting (Terrain.shader) is the Forward path's and does not draw here.
    //
    // No Domain: this program is chosen by the TerrainRenderer for the render path, never assigned by a
    // user; a terrain's .demat names `Terrain`. The Properties block is Terrain.shader's, so the material a
    // user edits drives both, and the ShaderCacheKey suite asserts the two blocks are the same.

    Properties Binding(1)
    {
        Color       Tint ("Tint") = (1, 1, 1, 1)
    }

    State
    {
        Topology Triangles
        Cull None
        ZTest Less
        ZWrite On
    }

    Vertex
    {
        #include <Programs/Terrain/TerrainVertex.glslh>
    }

    Fragment
    {
        // The G-buffer's four targets, as StaticMeshGBuffer.shader writes them.
        Out(0) vec4 oGBufferA;        // Albedo.rgb, Metallic.a
        Out(1) vec4 oGBufferB;        // Normal.rgb, Roughness.a
        Out(2) vec4 oGBufferC;        // WorldPosition.xyz, shading word.w (DefaultLit, no maps, no payload)
        Out(3) vec4 oGBufferEmissive; // Emissive.rgb

        #include <Programs/Terrain/TerrainSurface.glslh>
        #include <ShadingModels/ShadingModels.generated.glslh>

        void main()
        {
            TerrainSurface s = EvaluateTerrainSurface();

            // Ground is a matte dielectric, as the forward program's diffuse-only lighting draws it.
            const float roughness = 0.9;

            oGBufferA        = vec4( s.Albedo * u_Material.Tint.rgb, 0.0 );
            oGBufferB        = vec4( s.N, roughness );
            oGBufferC        = vec4( v_WorldPos, DesertPackShadingWord( SHADING_MODEL_INDEX_DEFAULT_LIT, 0.0,
                                                                            DesertPayload( 0.0, 0.0 ) ) );
            oGBufferEmissive = vec4( 0.0, 0.0, 0.0, 1.0 ); // no emission; material AO 1 (the ground has none)
        }
    }
}
