// DesertAsset {"Kind":"Shader","Guid":"c419b18ec806492b95736d52a750f4d0","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TerrainVirtualShadow"
{
    // The terrain as a virtual shadow map caster ("VSM: Raster", MeshRenderer::AddVirtualShadowRasterPasses through
    // IShadowCaster::DeclareVirtualShadow / RecordVirtualShadow): TerrainShadow.shader's vertex stage - the camera's
    // grid, LOD and morph, projected with the pushed clipmap level matrix - and a fragment that writes the physical
    // pool (Common/VirtualShadowRaster.glslh) instead of a depth attachment. The pass has no attachment; the
    // TerrainRenderer builds the pipeline without depth test or write and with depth clamp, like the mesh casters'.

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
        ZWrite Off
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
        Uniform(12) sampler WeightmapSampler;
        #define WeightmapTexture sampler2D(WeightmapTexture, WeightmapSampler)

        #include <Common/LandscapeWeights.glslh>
        #include <Programs/Terrain/TerrainCasterHole.glslh>
        #include <Common/VirtualShadowRaster.glslh>

        void main()
        {
            // A hole casts no shadow: the same visibility test as the surface (LandscapeIsHole).
            if ( TerrainCasterIsHole( v_TileSample ) )
                discard;
            VsmRasterWrite();
        }
    }
}
