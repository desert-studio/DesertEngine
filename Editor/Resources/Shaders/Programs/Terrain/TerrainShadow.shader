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

    // The tile's maps, as in Terrain.shader: material parameters the TerrainRenderer writes per tile. A
    // textures-only block generates no declaration (no Binding/TextureBinding); the samplers are
    // TerrainVertex.glslh's and this file's own.
    Properties
    {
        Texture2D   HeightmapTexture ("Heightmap", EngineSet)
        Texture2D   WeightmapTexture ("Weightmap", EngineSet)
    }

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

        // The row the vertex stage read, named by the pushed MaterialIndex. This program has no Properties
        // block, so nothing injects the transport into this stage: it is included here, as the vertex stage does.
        #include <Common/MaterialTransport.glslh>
        #include <Common/TerrainInstance.glslh>
        ReadBuffer(8) TerrainInstances
        {
            TerrainInstance Terrains[];
        };
        #define CurrentTerrain Terrains[m_PushConstants.MaterialIndex]

        // The tile's weightmap, as TerrainSurface.glslh reads it; bound only when the tile has layers
        // (Params2.y > 0), the backend's white fallback otherwise.
        // A bound texture property: a separate texture read through its own sampler state `<name>Sampler`,
        // which the material writes from the property (VulkanMaterialBackend::ApplyTexture2D).
        Uniform(10) texture2D WeightmapTexture;
        Uniform(12) sampler WeightmapTextureSampler;
        #define WeightmapTexture sampler2D(WeightmapTexture, WeightmapTextureSampler)

        #include <Common/LandscapeWeights.glslh>
        #include <Programs/Terrain/TerrainCasterHole.glslh>

        void main()
        {
            // A hole casts no shadow: the same visibility test as the surface (LandscapeIsHole).
            if ( TerrainCasterIsHole( v_TileSample ) )
                discard;
            // No depth written here: the whole bias is in the vertex stage's clip depth (TerrainVertex.glslh,
            // Common/ShadowCasterBias.glslh), so the rasterised depth is the one stored and early depth stays on.
        }
    }
}
