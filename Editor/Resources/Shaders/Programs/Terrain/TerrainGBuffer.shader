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

        // THE LANDSCAPE MATERIAL (UE's LandscapeLayerBlend inputs): Terrain.shader says what the inputs are.
        Float       FarTilingDistance ("Far Tiling Distance (cm)", Range(100,100000), Category("Layers")) = 3000
        Float       FarTilingScale ("Far Tiling Scale", Range(1,16), Category("Layers")) = 4
        Float       MacroVariationSize ("Macro Variation Size (cm)", Range(100,100000), Category("Layers")) = 6000
        Float       MacroVariationStrength ("Macro Variation Strength", Range(0,1), Category("Layers")) = 0.3
        LandscapeLayerInfo Layer0Layer ("Layer 0 Layer", Category("Layer 0"), Element(Layers, 0))
        Float       Layer0Blend ("Layer 0 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 0")) = 0
        Float       Layer0TileSize ("Layer 0 Tile Size (cm)", Range(10,10000), Category("Layer 0")) = 300
        Color       Layer0Tint ("Layer 0 Tint", Category("Layer 0")) = (1, 1, 1, 1)
        Texture2D   u_Layer0Albedo ("Layer 0 Albedo", Category("Layer 0"), Intent(Colour), Element(u_LayerAlbedo, 0))
        Texture2D   u_Layer0Normal ("Layer 0 Normal", Category("Layer 0"), Intent(NormalMap), Element(u_LayerNormal, 0))
        Texture2D   u_Layer0ARM ("Layer 0 ARM", Category("Layer 0"), Intent(Data), Element(u_LayerARM, 0))
        Texture2D   u_Layer0Height ("Layer 0 Height", Category("Layer 0"), Intent(Data), Element(u_LayerHeight, 0))
        LandscapeLayerInfo Layer1Layer ("Layer 1 Layer", Category("Layer 1"), Element(Layers, 1))
        Float       Layer1Blend ("Layer 1 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 1")) = 0
        Float       Layer1TileSize ("Layer 1 Tile Size (cm)", Range(10,10000), Category("Layer 1")) = 300
        Color       Layer1Tint ("Layer 1 Tint", Category("Layer 1")) = (1, 1, 1, 1)
        Texture2D   u_Layer1Albedo ("Layer 1 Albedo", Category("Layer 1"), Intent(Colour), Element(u_LayerAlbedo, 1))
        Texture2D   u_Layer1Normal ("Layer 1 Normal", Category("Layer 1"), Intent(NormalMap), Element(u_LayerNormal, 1))
        Texture2D   u_Layer1ARM ("Layer 1 ARM", Category("Layer 1"), Intent(Data), Element(u_LayerARM, 1))
        Texture2D   u_Layer1Height ("Layer 1 Height", Category("Layer 1"), Intent(Data), Element(u_LayerHeight, 1))
        LandscapeLayerInfo Layer2Layer ("Layer 2 Layer", Category("Layer 2"), Element(Layers, 2))
        Float       Layer2Blend ("Layer 2 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 2")) = 0
        Float       Layer2TileSize ("Layer 2 Tile Size (cm)", Range(10,10000), Category("Layer 2")) = 300
        Color       Layer2Tint ("Layer 2 Tint", Category("Layer 2")) = (1, 1, 1, 1)
        Texture2D   u_Layer2Albedo ("Layer 2 Albedo", Category("Layer 2"), Intent(Colour), Element(u_LayerAlbedo, 2))
        Texture2D   u_Layer2Normal ("Layer 2 Normal", Category("Layer 2"), Intent(NormalMap), Element(u_LayerNormal, 2))
        Texture2D   u_Layer2ARM ("Layer 2 ARM", Category("Layer 2"), Intent(Data), Element(u_LayerARM, 2))
        Texture2D   u_Layer2Height ("Layer 2 Height", Category("Layer 2"), Intent(Data), Element(u_LayerHeight, 2))
        LandscapeLayerInfo Layer3Layer ("Layer 3 Layer", Category("Layer 3"), Element(Layers, 3))
        Float       Layer3Blend ("Layer 3 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 3")) = 0
        Float       Layer3TileSize ("Layer 3 Tile Size (cm)", Range(10,10000), Category("Layer 3")) = 300
        Color       Layer3Tint ("Layer 3 Tint", Category("Layer 3")) = (1, 1, 1, 1)
        Texture2D   u_Layer3Albedo ("Layer 3 Albedo", Category("Layer 3"), Intent(Colour), Element(u_LayerAlbedo, 3))
        Texture2D   u_Layer3Normal ("Layer 3 Normal", Category("Layer 3"), Intent(NormalMap), Element(u_LayerNormal, 3))
        Texture2D   u_Layer3ARM ("Layer 3 ARM", Category("Layer 3"), Intent(Data), Element(u_LayerARM, 3))
        Texture2D   u_Layer3Height ("Layer 3 Height", Category("Layer 3"), Intent(Data), Element(u_LayerHeight, 3))
        LandscapeLayerInfo Layer4Layer ("Layer 4 Layer", Category("Layer 4"), Element(Layers, 4))
        Float       Layer4Blend ("Layer 4 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 4")) = 0
        Float       Layer4TileSize ("Layer 4 Tile Size (cm)", Range(10,10000), Category("Layer 4")) = 300
        Color       Layer4Tint ("Layer 4 Tint", Category("Layer 4")) = (1, 1, 1, 1)
        Texture2D   u_Layer4Albedo ("Layer 4 Albedo", Category("Layer 4"), Intent(Colour), Element(u_LayerAlbedo, 4))
        Texture2D   u_Layer4Normal ("Layer 4 Normal", Category("Layer 4"), Intent(NormalMap), Element(u_LayerNormal, 4))
        Texture2D   u_Layer4ARM ("Layer 4 ARM", Category("Layer 4"), Intent(Data), Element(u_LayerARM, 4))
        Texture2D   u_Layer4Height ("Layer 4 Height", Category("Layer 4"), Intent(Data), Element(u_LayerHeight, 4))
        LandscapeLayerInfo Layer5Layer ("Layer 5 Layer", Category("Layer 5"), Element(Layers, 5))
        Float       Layer5Blend ("Layer 5 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 5")) = 0
        Float       Layer5TileSize ("Layer 5 Tile Size (cm)", Range(10,10000), Category("Layer 5")) = 300
        Color       Layer5Tint ("Layer 5 Tint", Category("Layer 5")) = (1, 1, 1, 1)
        Texture2D   u_Layer5Albedo ("Layer 5 Albedo", Category("Layer 5"), Intent(Colour), Element(u_LayerAlbedo, 5))
        Texture2D   u_Layer5Normal ("Layer 5 Normal", Category("Layer 5"), Intent(NormalMap), Element(u_LayerNormal, 5))
        Texture2D   u_Layer5ARM ("Layer 5 ARM", Category("Layer 5"), Intent(Data), Element(u_LayerARM, 5))
        Texture2D   u_Layer5Height ("Layer 5 Height", Category("Layer 5"), Intent(Data), Element(u_LayerHeight, 5))
        LandscapeLayerInfo Layer6Layer ("Layer 6 Layer", Category("Layer 6"), Element(Layers, 6))
        Float       Layer6Blend ("Layer 6 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 6")) = 0
        Float       Layer6TileSize ("Layer 6 Tile Size (cm)", Range(10,10000), Category("Layer 6")) = 300
        Color       Layer6Tint ("Layer 6 Tint", Category("Layer 6")) = (1, 1, 1, 1)
        Texture2D   u_Layer6Albedo ("Layer 6 Albedo", Category("Layer 6"), Intent(Colour), Element(u_LayerAlbedo, 6))
        Texture2D   u_Layer6Normal ("Layer 6 Normal", Category("Layer 6"), Intent(NormalMap), Element(u_LayerNormal, 6))
        Texture2D   u_Layer6ARM ("Layer 6 ARM", Category("Layer 6"), Intent(Data), Element(u_LayerARM, 6))
        Texture2D   u_Layer6Height ("Layer 6 Height", Category("Layer 6"), Intent(Data), Element(u_LayerHeight, 6))
        LandscapeLayerInfo Layer7Layer ("Layer 7 Layer", Category("Layer 7"), Element(Layers, 7))
        Float       Layer7Blend ("Layer 7 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 7")) = 0
        Float       Layer7TileSize ("Layer 7 Tile Size (cm)", Range(10,10000), Category("Layer 7")) = 300
        Color       Layer7Tint ("Layer 7 Tint", Category("Layer 7")) = (1, 1, 1, 1)
        Texture2D   u_Layer7Albedo ("Layer 7 Albedo", Category("Layer 7"), Intent(Colour), Element(u_LayerAlbedo, 7))
        Texture2D   u_Layer7Normal ("Layer 7 Normal", Category("Layer 7"), Intent(NormalMap), Element(u_LayerNormal, 7))
        Texture2D   u_Layer7ARM ("Layer 7 ARM", Category("Layer 7"), Intent(Data), Element(u_LayerARM, 7))
        Texture2D   u_Layer7Height ("Layer 7 Height", Category("Layer 7"), Intent(Data), Element(u_LayerHeight, 7))

        // The four arrays the inputs' maps are copied into, slice by slice (TerrainRenderer: an array per map kind,
        // every slice of one kind at one size and format). Engine-built from the Element textures above, never
        // authored; declared by hand in TerrainSurface.glslh as sampler2DArray.
        Texture2DArray u_LayerAlbedo ("Layer Albedo", EngineSet)
        Texture2DArray u_LayerNormal ("Layer Normal", EngineSet)
        Texture2DArray u_LayerARM ("Layer ARM", EngineSet)
        Texture2DArray u_LayerHeight ("Layer Height", EngineSet)

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
