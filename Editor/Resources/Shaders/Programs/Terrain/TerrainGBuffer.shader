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

        // THE LANDSCAPE MATERIAL (UE's LandscapeLayerBlend inputs): input k is the root's layer k. Its maps are
        // sampled with world-aligned UVs at the input's tile size (cm per repeat); TerrainSurface.glslh.
        // An input without an albedo map draws its layer's LayerUsageDebugColor.
        Float       HeightBlend ("Height Blend", Range(0,1), Category("Layers")) = 1
        Float       FarTilingDistance ("Far Tiling Distance (cm)", Range(100,100000), Category("Layers")) = 3000
        Float       FarTilingScale ("Far Tiling Scale", Range(1,16), Category("Layers")) = 4
        Float       MacroVariationSize ("Macro Variation Size (cm)", Range(100,100000), Category("Layers")) = 6000
        Float       MacroVariationStrength ("Macro Variation Strength", Range(0,1), Category("Layers")) = 0.3
        Float       Layer0TileSize ("Layer 0 Tile Size (cm)", Range(10,10000), Category("Layer 0")) = 300
        Color       Layer0Tint ("Layer 0 Tint", Category("Layer 0")) = (1, 1, 1, 1)
        Texture2D   u_Layer0Albedo ("Layer 0 Albedo", Category("Layer 0"), Intent(Colour))
        Texture2D   u_Layer0Normal ("Layer 0 Normal", Category("Layer 0"), Intent(NormalMap)) = "normal"
        Texture2D   u_Layer0ARM ("Layer 0 ARM", Category("Layer 0"), Intent(Data))
        Texture2D   u_Layer0Height ("Layer 0 Height", Category("Layer 0"), Intent(Data))
        Float       Layer1TileSize ("Layer 1 Tile Size (cm)", Range(10,10000), Category("Layer 1")) = 300
        Color       Layer1Tint ("Layer 1 Tint", Category("Layer 1")) = (1, 1, 1, 1)
        Texture2D   u_Layer1Albedo ("Layer 1 Albedo", Category("Layer 1"), Intent(Colour))
        Texture2D   u_Layer1Normal ("Layer 1 Normal", Category("Layer 1"), Intent(NormalMap)) = "normal"
        Texture2D   u_Layer1ARM ("Layer 1 ARM", Category("Layer 1"), Intent(Data))
        Texture2D   u_Layer1Height ("Layer 1 Height", Category("Layer 1"), Intent(Data))
        Float       Layer2TileSize ("Layer 2 Tile Size (cm)", Range(10,10000), Category("Layer 2")) = 300
        Color       Layer2Tint ("Layer 2 Tint", Category("Layer 2")) = (1, 1, 1, 1)
        Texture2D   u_Layer2Albedo ("Layer 2 Albedo", Category("Layer 2"), Intent(Colour))
        Texture2D   u_Layer2Normal ("Layer 2 Normal", Category("Layer 2"), Intent(NormalMap)) = "normal"
        Texture2D   u_Layer2ARM ("Layer 2 ARM", Category("Layer 2"), Intent(Data))
        Texture2D   u_Layer2Height ("Layer 2 Height", Category("Layer 2"), Intent(Data))

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

    Vertex
    {
        #include <Programs/Terrain/TerrainVertex.glslh>
    }

    Fragment
    {
        // The G-buffer's four targets, as StaticMeshGBuffer.shader writes them.
        Out(0) vec4 oGBufferA;        // Albedo.rgb, Metallic.a
        Out(1) vec4 oGBufferB;        // Normal.rgb, Roughness.a
        Out(2) uvec2 oGBufferShadingWord; // shading word (DefaultLit, no maps, no payload); no world position
        Out(3) vec4 oGBufferEmissive; // Emissive.rgb
        Out(4) vec2 oVelocity;        // the view's velocity, slot 4 of the G-buffer target (TerrainSurface.glslh)

        #include <Programs/Terrain/TerrainSurface.glslh>
        #include <Generated/ShadingModels.glslh>

        void main()
        {
            TerrainSurface s = EvaluateTerrainSurface();

            // Ground is a dielectric; roughness and occlusion are the landscape material's layers'.
            oGBufferA        = vec4( s.Albedo * u_Material.Tint.rgb, 0.0 );
            oGBufferB        = vec4( s.N, s.Roughness );
            oGBufferShadingWord = DesertPackShadingWord( SHADING_MODEL_INDEX_DEFAULT_LIT, 0.0, 0.5, DesertPayload( 0.0, 0.0, 0.0, 0.0 ) );
            oGBufferEmissive = vec4( 0.0, 0.0, 0.0, s.AO ); // no emission; material AO from the layers' ARM maps
            oVelocity        = TerrainVelocity();
        }
    }
}
