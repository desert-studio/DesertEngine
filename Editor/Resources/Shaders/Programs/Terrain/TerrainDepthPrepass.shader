// DesertAsset {"Kind":"Shader","Guid":"98784bcb9091461aabd3d87a851537fd","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TerrainDepthPrepass"
{
    // The terrain in the FORWARD path's depth prepass (UE FDepthPassMeshProcessor): the camera's depth before the
    // base pass, which then draws Terrain.shader testing EQUAL against it and writing none. The vertex stage is the
    // forward program's own (TerrainVertex.glslh, `invariant gl_Position`, the same pushed jittered matrix), so the
    // two programs place every vertex at the same bits. The one colour target is the opaque scene's shading word
    // with only the contact-shadow cast bit (Mesh/Surface/Pass_DepthPrepass.glslh): the ground always casts.
    //
    // No Domain: chosen by the TerrainRenderer for the pass, never assigned by a user. The tile's maps are material
    // parameters the TerrainRenderer writes per tile, as in TerrainShadow.shader: a textures-only block generates
    // no declaration; the samplers are TerrainVertex.glslh's and this file's own.

    Properties
    {
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

    Vertex
    {
        #include <Programs/Terrain/TerrainVertex.glslh>
    }

    Fragment
    {
        In(0) vec3 v_WorldPos;

        // The row the vertex stage read, named by the pushed MaterialIndex. No Properties binding injects the
        // transport into this stage: it is included here, as the vertex stage does.
        #include <Common/MaterialTransport.glslh>
        #include <Common/TerrainInstance.glslh>
        ReadBuffer(8) TerrainInstances
        {
            TerrainInstance u_Terrains[];
        };
        #define u_T u_Terrains[m_PushConstants.MaterialIndex]

        // The tile's weightmap, as TerrainSurface.glslh reads it; bound only when the tile has layers
        // (Params2.y > 0), the backend's white fallback otherwise.
        Uniform(10) sampler2D u_Weightmap;

        #include <Common/LandscapeWeights.glslh>
        #include <ShadingModels/ShadingModelContract.glslh>

        Out(0) uvec2 oContactShadowWord;

        void main()
        {
            // A hole has no depth: the same visibility test, on the same sample coordinate, as the surface
            // (TerrainSurface.glslh EvaluateTerrainSurface) and the caster (TerrainShadow.shader).
            if ( u_T.Params2.y > 0.5 && u_T.Params.z > 0.5 )
            {
                vec2  g     = ( v_WorldPos.xz - u_T.LandscapeFrame.xz ) / u_T.LandscapeFrame.w - u_T.LandscapeTile.xy;
                vec2  size  = vec2( textureSize( u_Weightmap, 0 ) );
                float pages = u_T.Params2.z;
                vec4  w0    = texture( u_Weightmap, LandscapeWeightmapPageUV( g, 0.0, size, pages ) );
                vec4  w1    = pages > 1.5 ? texture( u_Weightmap, LandscapeWeightmapPageUV( g, 1.0, size, pages ) )
                                          : vec4( 0.0 );
                if ( LandscapeIsHole( LandscapeVisibility( w0, w1, u_T.Params.z ) ) )
                    discard;
            }
            oContactShadowWord = DesertMarkContactShadowCast( uvec2( 0u ), true );
        }
    }
}
