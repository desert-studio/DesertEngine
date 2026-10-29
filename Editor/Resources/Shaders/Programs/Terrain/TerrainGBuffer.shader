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
        Float       DetailNormalScale ("Detail Normal Scale (x tile size)", Range(1,64), Category("Layers")) = 1
        Float       DetailNormalStrength ("Detail Normal Strength", Range(0,4), Category("Layers")) = 0
        Float       MacroHueSize ("Macro Hue Size (cm)", Range(100,100000), Category("Layers")) = 6000
        Float       MacroHueStrength ("Macro Hue Strength (radians)", Range(0,1.5), Category("Layers")) = 0
        // DISPLACEMENT (UE 5.4+ landscape displacement: the material's Displacement output, LandscapeDisplacement.glslh):
        // the layers' blended height moves the ground by (height - Center) x Magnitude along its normal, full up to
        // FadeStart from the camera and gone by FadeEnd. 0 cm = a flat ground; the dicing follows Scalability
        // ViewDistance.DisplacementDicingRate, as mesh displacement does (0 = off).
        Float       DisplacementMagnitude ("Displacement Magnitude (cm)", Range(0,200), Category("Displacement")) = 0
        Float       DisplacementCenter ("Displacement Center", Range(0,1), Category("Displacement")) = 0.5
        Float       DisplacementFadeStart ("Displacement Fade Start (cm)", Range(100,20000), Category("Displacement")) = 1500
        Float       DisplacementFadeEnd ("Displacement Fade End (cm)", Range(200,30000), Category("Displacement")) = 2500
        LandscapeLayerInfo Layer0Layer ("Layer 0 Layer", Category("Layer 0"), Element(Layers, 0))
        Int         Layer0Blend ("Layer 0 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 0")) = 0
        Float       Layer0TileSize ("Layer 0 Tile Size (cm)", Range(10,10000), Category("Layer 0")) = 300
        Color       Layer0Tint ("Layer 0 Tint", Category("Layer 0")) = (1, 1, 1, 1)
        Float       Layer0NormalStrength ("Layer 0 Normal Strength", Range(0,4), Category("Layer 0")) = 1
        Texture2D   Layer0AlbedoTexture ("Layer 0 Albedo", Category("Layer 0"), Intent(Colour), Element(LayerAlbedoTexture, 0))
        Texture2D   Layer0NormalTexture ("Layer 0 Normal", Category("Layer 0"), Intent(NormalMap), Element(LayerNormalTexture, 0))
        Texture2D   Layer0ArmTexture ("Layer 0 ARM", Category("Layer 0"), Intent(Data), Element(LayerArmTexture, 0))
        Texture2D   Layer0HeightTexture ("Layer 0 Height", Category("Layer 0"), Intent(Data), Element(LayerHeightTexture, 0))
        LandscapeLayerInfo Layer1Layer ("Layer 1 Layer", Category("Layer 1"), Element(Layers, 1))
        Int         Layer1Blend ("Layer 1 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 1")) = 0
        Float       Layer1TileSize ("Layer 1 Tile Size (cm)", Range(10,10000), Category("Layer 1")) = 300
        Color       Layer1Tint ("Layer 1 Tint", Category("Layer 1")) = (1, 1, 1, 1)
        Float       Layer1NormalStrength ("Layer 1 Normal Strength", Range(0,4), Category("Layer 1")) = 1
        Texture2D   Layer1AlbedoTexture ("Layer 1 Albedo", Category("Layer 1"), Intent(Colour), Element(LayerAlbedoTexture, 1))
        Texture2D   Layer1NormalTexture ("Layer 1 Normal", Category("Layer 1"), Intent(NormalMap), Element(LayerNormalTexture, 1))
        Texture2D   Layer1ArmTexture ("Layer 1 ARM", Category("Layer 1"), Intent(Data), Element(LayerArmTexture, 1))
        Texture2D   Layer1HeightTexture ("Layer 1 Height", Category("Layer 1"), Intent(Data), Element(LayerHeightTexture, 1))
        LandscapeLayerInfo Layer2Layer ("Layer 2 Layer", Category("Layer 2"), Element(Layers, 2))
        Int         Layer2Blend ("Layer 2 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 2")) = 0
        Float       Layer2TileSize ("Layer 2 Tile Size (cm)", Range(10,10000), Category("Layer 2")) = 300
        Color       Layer2Tint ("Layer 2 Tint", Category("Layer 2")) = (1, 1, 1, 1)
        Float       Layer2NormalStrength ("Layer 2 Normal Strength", Range(0,4), Category("Layer 2")) = 1
        Texture2D   Layer2AlbedoTexture ("Layer 2 Albedo", Category("Layer 2"), Intent(Colour), Element(LayerAlbedoTexture, 2))
        Texture2D   Layer2NormalTexture ("Layer 2 Normal", Category("Layer 2"), Intent(NormalMap), Element(LayerNormalTexture, 2))
        Texture2D   Layer2ArmTexture ("Layer 2 ARM", Category("Layer 2"), Intent(Data), Element(LayerArmTexture, 2))
        Texture2D   Layer2HeightTexture ("Layer 2 Height", Category("Layer 2"), Intent(Data), Element(LayerHeightTexture, 2))
        LandscapeLayerInfo Layer3Layer ("Layer 3 Layer", Category("Layer 3"), Element(Layers, 3))
        Int         Layer3Blend ("Layer 3 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 3")) = 0
        Float       Layer3TileSize ("Layer 3 Tile Size (cm)", Range(10,10000), Category("Layer 3")) = 300
        Color       Layer3Tint ("Layer 3 Tint", Category("Layer 3")) = (1, 1, 1, 1)
        Float       Layer3NormalStrength ("Layer 3 Normal Strength", Range(0,4), Category("Layer 3")) = 1
        Texture2D   Layer3AlbedoTexture ("Layer 3 Albedo", Category("Layer 3"), Intent(Colour), Element(LayerAlbedoTexture, 3))
        Texture2D   Layer3NormalTexture ("Layer 3 Normal", Category("Layer 3"), Intent(NormalMap), Element(LayerNormalTexture, 3))
        Texture2D   Layer3ArmTexture ("Layer 3 ARM", Category("Layer 3"), Intent(Data), Element(LayerArmTexture, 3))
        Texture2D   Layer3HeightTexture ("Layer 3 Height", Category("Layer 3"), Intent(Data), Element(LayerHeightTexture, 3))
        LandscapeLayerInfo Layer4Layer ("Layer 4 Layer", Category("Layer 4"), Element(Layers, 4))
        Int         Layer4Blend ("Layer 4 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 4")) = 0
        Float       Layer4TileSize ("Layer 4 Tile Size (cm)", Range(10,10000), Category("Layer 4")) = 300
        Color       Layer4Tint ("Layer 4 Tint", Category("Layer 4")) = (1, 1, 1, 1)
        Float       Layer4NormalStrength ("Layer 4 Normal Strength", Range(0,4), Category("Layer 4")) = 1
        Texture2D   Layer4AlbedoTexture ("Layer 4 Albedo", Category("Layer 4"), Intent(Colour), Element(LayerAlbedoTexture, 4))
        Texture2D   Layer4NormalTexture ("Layer 4 Normal", Category("Layer 4"), Intent(NormalMap), Element(LayerNormalTexture, 4))
        Texture2D   Layer4ArmTexture ("Layer 4 ARM", Category("Layer 4"), Intent(Data), Element(LayerArmTexture, 4))
        Texture2D   Layer4HeightTexture ("Layer 4 Height", Category("Layer 4"), Intent(Data), Element(LayerHeightTexture, 4))
        LandscapeLayerInfo Layer5Layer ("Layer 5 Layer", Category("Layer 5"), Element(Layers, 5))
        Int         Layer5Blend ("Layer 5 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 5")) = 0
        Float       Layer5TileSize ("Layer 5 Tile Size (cm)", Range(10,10000), Category("Layer 5")) = 300
        Color       Layer5Tint ("Layer 5 Tint", Category("Layer 5")) = (1, 1, 1, 1)
        Float       Layer5NormalStrength ("Layer 5 Normal Strength", Range(0,4), Category("Layer 5")) = 1
        Texture2D   Layer5AlbedoTexture ("Layer 5 Albedo", Category("Layer 5"), Intent(Colour), Element(LayerAlbedoTexture, 5))
        Texture2D   Layer5NormalTexture ("Layer 5 Normal", Category("Layer 5"), Intent(NormalMap), Element(LayerNormalTexture, 5))
        Texture2D   Layer5ArmTexture ("Layer 5 ARM", Category("Layer 5"), Intent(Data), Element(LayerArmTexture, 5))
        Texture2D   Layer5HeightTexture ("Layer 5 Height", Category("Layer 5"), Intent(Data), Element(LayerHeightTexture, 5))
        LandscapeLayerInfo Layer6Layer ("Layer 6 Layer", Category("Layer 6"), Element(Layers, 6))
        Int         Layer6Blend ("Layer 6 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 6")) = 0
        Float       Layer6TileSize ("Layer 6 Tile Size (cm)", Range(10,10000), Category("Layer 6")) = 300
        Color       Layer6Tint ("Layer 6 Tint", Category("Layer 6")) = (1, 1, 1, 1)
        Float       Layer6NormalStrength ("Layer 6 Normal Strength", Range(0,4), Category("Layer 6")) = 1
        Texture2D   Layer6AlbedoTexture ("Layer 6 Albedo", Category("Layer 6"), Intent(Colour), Element(LayerAlbedoTexture, 6))
        Texture2D   Layer6NormalTexture ("Layer 6 Normal", Category("Layer 6"), Intent(NormalMap), Element(LayerNormalTexture, 6))
        Texture2D   Layer6ArmTexture ("Layer 6 ARM", Category("Layer 6"), Intent(Data), Element(LayerArmTexture, 6))
        Texture2D   Layer6HeightTexture ("Layer 6 Height", Category("Layer 6"), Intent(Data), Element(LayerHeightTexture, 6))
        LandscapeLayerInfo Layer7Layer ("Layer 7 Layer", Category("Layer 7"), Element(Layers, 7))
        Int         Layer7Blend ("Layer 7 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 7")) = 0
        Float       Layer7TileSize ("Layer 7 Tile Size (cm)", Range(10,10000), Category("Layer 7")) = 300
        Color       Layer7Tint ("Layer 7 Tint", Category("Layer 7")) = (1, 1, 1, 1)
        Float       Layer7NormalStrength ("Layer 7 Normal Strength", Range(0,4), Category("Layer 7")) = 1
        Texture2D   Layer7AlbedoTexture ("Layer 7 Albedo", Category("Layer 7"), Intent(Colour), Element(LayerAlbedoTexture, 7))
        Texture2D   Layer7NormalTexture ("Layer 7 Normal", Category("Layer 7"), Intent(NormalMap), Element(LayerNormalTexture, 7))
        Texture2D   Layer7ArmTexture ("Layer 7 ARM", Category("Layer 7"), Intent(Data), Element(LayerArmTexture, 7))
        Texture2D   Layer7HeightTexture ("Layer 7 Height", Category("Layer 7"), Intent(Data), Element(LayerHeightTexture, 7))

        // The four arrays the inputs' maps are copied into, slice by slice (TerrainRenderer: an array per map kind,
        // every slice of one kind at one size and format). Engine-built from the Element textures above, never
        // authored; declared by hand in TerrainSurface.glslh as sampler2DArray.
        Texture2DArray LayerAlbedoTexture ("Layer Albedo", EngineSet)
        Texture2DArray LayerNormalTexture ("Layer Normal", EngineSet)
        Texture2DArray LayerArmTexture ("Layer ARM", EngineSet)
        Texture2DArray LayerHeightTexture ("Layer Height", EngineSet)

        // The landscape tile's own maps (LandscapeECSSystem's R16 height / RGBA8 weight copies): material
        // parameters the TerrainRenderer writes per tile, never an artist (EngineSet). Declared by hand in
        // TerrainVertex.glslh / TerrainSurface.glslh; a Binding(1) row skips textures.
        Texture2D   HeightmapTexture ("Heightmap", EngineSet)
        Texture2D   WeightmapTexture ("Weightmap", EngineSet)
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
        Out(2) uvec2 oGBufferShadingWord; // shading word (DefaultLit; TwoSidedFoliage under the grass cover); no world position
        Out(3) vec4 oGBufferEmissive; // Emissive.rgb
        Out(4) vec4 oVelocity;        // the view's velocity, slot 4 of the G-buffer target (TerrainSurface.glslh)
        Out(5) float oThinGeometryCoverage; // slot 5 of the G-buffer target (Common/ThinGeometryCoverage.glslh)

        #include <Programs/Terrain/TerrainSurface.glslh>
        #include <Generated/ShadingModels.glslh>
        #include <Common/ThinGeometryCoverage.glslh>

        void main()
        {
            vec3           transmission;
            float          cover;
            TerrainSurface s = EvaluateTerrainSurfaceCovered( Material.Tint.rgb, transmission, cover );

            // Ground is a dielectric; roughness and occlusion are the landscape material's layers'. Where the grass
            // field's far cover lies over it (GRASS-FIELD) the pixel shades as the blades do - TwoSidedFoliage, its
            // SubsurfaceColor the cover's share of the blades' (stored as its square root, UE
            // EncodeSubsurfaceColor), so the transmitted light fades in with the cover and bare ground stays
            // DefaultLit.
            oGBufferA        = vec4( s.Albedo, 0.0 );
            oGBufferB        = vec4( s.N, s.Roughness );
            const vec3 sss   = sqrt( clamp( transmission, 0.0, 1.0 ) );
            oGBufferShadingWord = cover > 0.0
                                       ? DesertPackShadingWord( SHADING_MODEL_INDEX_TWO_SIDED_FOLIAGE, 0.0, 0.5,
                                                                DesertPayload( sss.r, sss.g, sss.b, cover ) )
                                       : DesertPackShadingWord( SHADING_MODEL_INDEX_DEFAULT_LIT, 0.0, 0.5,
                                                                DesertPayload( 0.0, 0.0, 0.0, 0.0 ) );
            oGBufferEmissive = vec4( 0.0, 0.0, 0.0, s.AO ); // no emission; material AO from the layers' ARM maps
            oVelocity        = TerrainVelocity();
            oThinGeometryCoverage = DesertThinGeometryCoverage( SHADING_MODEL_INDEX_DEFAULT_LIT );
        }
    }
}
