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
        Float       DetailTiling ("Texture Tiling (m)", Range(0.25,64)) = 4
        Texture2D   u_GrassTex ("Grass Texture")
        Texture2D   u_RockTex ("Rock Texture")
        Texture2D   u_SnowTex ("Snow Texture")
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
        Out(2) vec4 oGBufferC;        // WorldPosition.xyz, texCount.w
        Out(3) vec4 oGBufferEmissive; // Emissive.rgb

        #include <Programs/Terrain/TerrainSurface.glslh>

        void main()
        {
            TerrainSurface s = EvaluateTerrainSurface();

            // Ground is a dielectric. Rock and grass are matte; snow is the one layer with a sheen — the
            // forward program's Blinn gloss 0..0.5 over the snow cover, expressed as a roughness.
            float roughness = mix( 0.9, 0.45, s.Snow );

            // The complexity proxy the deferred debug view heat-maps: the three layer maps.
            int texCount = 0;
            if ( textureSize( u_GrassTex, 0 ).x > 1 ) texCount++;
            if ( textureSize( u_RockTex, 0 ).x > 1 ) texCount++;
            if ( textureSize( u_SnowTex, 0 ).x > 1 ) texCount++;

            oGBufferA        = vec4( s.Albedo * u_Material.Tint.rgb, 0.0 );
            oGBufferB        = vec4( s.N, roughness );
            oGBufferC        = vec4( v_WorldPos, float( texCount ) );
            oGBufferEmissive = vec4( 0.0, 0.0, 0.0, 1.0 );
        }
    }
}
