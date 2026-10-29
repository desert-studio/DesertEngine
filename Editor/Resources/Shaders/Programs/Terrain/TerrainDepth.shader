// DesertAsset {"Kind":"Shader","Guid":"cdb0ef7b2d2d4483a1179d83e4727ccd","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TerrainDepth"
{
    // The terrain in the FORWARD view's depth prepass (UE: the DepthPass mesh pass, DepthOnlyPixelShader.usf):
    // the camera's own vertex stage (TerrainVertex.glslh, invariant gl_Position) into the scene depth before the
    // base pass, which then tests CloserOrEqual and writes none. A hole (the visibility layer) is discarded on the
    // same test as the surface's (TerrainCasterHole.glslh), so the base pass keeps exactly the texels it keeps.
    //
    // No Domain: chosen by the TerrainRenderer, never assigned by a user. No Properties row: only the tile's own
    // maps, which the renderer writes per tile.

    Properties
    {
        Texture2D   HeightmapTexture ("Heightmap", EngineSet)
        Texture2D   WeightmapTexture ("Weightmap", EngineSet)
        // The landscape material's layer height array: the displaced vertex reads it (LandscapeDisplacement.glslh).
        Texture2DArray LayerHeightTexture ("Layer Height", EngineSet)
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
        #define TERRAIN_VIEW_DEPTH
        #include <Programs/Terrain/TerrainVertex.glslh>
    }

    Fragment
    {
        In(0) vec2 v_TileSample;

        #include <Common/MaterialTransport.glslh>
        #include <Common/TerrainInstance.glslh>
        ReadBuffer(8) TerrainInstances
        {
            TerrainInstance Terrains[];
        };
        #define CurrentTerrain Terrains[m_PushConstants.MaterialIndex]

        Uniform(10) texture2D WeightmapTexture;
        Uniform(12) sampler WeightmapTextureSampler;
        #define WeightmapTexture sampler2D(WeightmapTexture, WeightmapTextureSampler)

        #include <Common/LandscapeWeights.glslh>
        #include <Programs/Terrain/TerrainCasterHole.glslh>

        void main()
        {
            if ( TerrainCasterIsHole( v_TileSample ) )
                discard;
        }
    }
}
