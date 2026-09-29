// DesertAsset {"Kind":"Shader","Guid":"62162bdefe547edc0dad159167dd342f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TerrainShadow"
{
    // The terrain as a cascade shadow caster: depth only, into the cascade targets the mesh casters write
    // (MeshRenderer's cascade pass records it through IShadowCaster). The grid, its LOD and its morph are the
    // camera pass's own vertex stage — and the LOD is the camera's, because the TerrainRenderer picks it from
    // the MAIN camera and passes it in the tile's row: a cascade sees exactly the triangles the camera sees,
    // so the shadow cannot swim or open a gap where a LOD change would move them. The vertex stage projects
    // with the pushed cascade matrix.
    //
    // No render state for depth compare: the cascades are STANDARD-Z (SetupShadowPass), so the
    // TerrainRenderer sets LessOrEqual itself; a `ZTest` here would be mirrored for the reversed-Z camera.

    State
    {
        Topology Triangles
        Cull None
        ZWrite On
    }

    Vertex
    {
        // Positions only: the normal and the height tint are for shading, and nothing shades a caster.
        #define TERRAIN_DEPTH_ONLY
        #include <Programs/Terrain/TerrainVertex.glslh>
    }

    Fragment
    {
        In(0) vec2 v_TileSample;
        Out(0) vec4 o_Depth;

        // The row the vertex stage read, named by the pushed MaterialIndex. This program has no Properties
        // block, so nothing injects the transport into this stage: it is included here, as the vertex stage does.
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

        void main()
        {
            // A hole casts no shadow: the same visibility test as the surface (LandscapeIsHole).
            if ( u_T.Params2.y > 0.5 && u_T.Params.z > 0.5 )
            {
                vec2  size  = vec2( textureSize( u_Weightmap, 0 ) );
                float pages = u_T.Params2.z;
                vec4  w0    = texture( u_Weightmap, LandscapeWeightmapPageUV( v_TileSample, 0.0, size, pages ) );
                vec4  w1    = pages > 1.5 ? texture( u_Weightmap, LandscapeWeightmapPageUV( v_TileSample, 1.0, size, pages ) )
                                          : vec4( 0.0 );
                if ( LandscapeIsHole( LandscapeVisibility( w0, w1, u_T.Params.z ) ) )
                    discard;
            }
            // Same encoding as Shadow.shader: the R32F-in-RGBA32F target the receivers sample.
            o_Depth = vec4( gl_FragCoord.z, 0.0, 0.0, 1.0 );
        }
    }
}
