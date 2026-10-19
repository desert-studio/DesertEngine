// DesertAsset {"Kind":"Shader","Guid":"9d6d6db70d7f0b397eac782bfb709efd","Versions":{"SHDR":1},"Dependencies":[]}
Shader "Terrain"
{
    // Data-driven material metadata: the landscape material. The layers' names and weights are the
    // landscape's painted layer infos; what each layer looks like is this material's layer inputs (UE's
    // LandscapeLayerBlend), plus the Tint every layer is multiplied by.

    Domain Terrain
    Role Terrain

    // Binding(1) is what makes these parameters a ROW of the shared `Materials[]` storage buffer, read
    // through `u_Material` in the fragment stage. The block used to be written out by hand below and was
    // a uniform block per material — one set of values for every terrain in the scene, which is the same
    // defect the graph materials had (Engine/Core/Formats/MaterialParamRow.hpp).
    Properties Binding(1)
    {
        Color       Tint ("Tint") = (1, 1, 1, 1)

        // THE LANDSCAPE MATERIAL (UE's LandscapeLayerBlend inputs): up to eight inputs, one per weight layer the
        // tiles can carry (kLandscapeMaxWeightLayers). Input k names its layer by its LandscapeLayerInfo and is
        // matched to every tile channel painted with that info's LayerName, in any order (UE binds by LayerName
        // too). Each input has its blend type (UE's ELandscapeLayerBlendType: 0 LB_WeightBlend, 1 LB_HeightBlend,
        // 2 LB_AlphaBlend), its maps sampled with world-aligned UVs at its tile size (cm per repeat) and its
        // Tint; TerrainSurface.glslh. A layer no input names, or whose input has no albedo map, draws its
        // LayerUsageDebugColor, flat.
        Float       FarTilingDistance ("Far Tiling Distance (cm)", Range(100,100000), Category("Layers")) = 3000
        Float       FarTilingScale ("Far Tiling Scale", Range(1,16), Category("Layers")) = 4
        Float       MacroVariationSize ("Macro Variation Size (cm)", Range(100,100000), Category("Layers")) = 6000
        Float       MacroVariationStrength ("Macro Variation Strength", Range(0,1), Category("Layers")) = 0.3
        LandscapeLayerInfo Layer0Layer ("Layer 0 Layer", Category("Layer 0"), Element(Layers, 0))
        Int         Layer0Blend ("Layer 0 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 0")) = 0
        Float       Layer0TileSize ("Layer 0 Tile Size (cm)", Range(10,10000), Category("Layer 0")) = 300
        Color       Layer0Tint ("Layer 0 Tint", Category("Layer 0")) = (1, 1, 1, 1)
        Texture2D   u_Layer0Albedo ("Layer 0 Albedo", Category("Layer 0"), Intent(Colour), Element(u_LayerAlbedo, 0))
        Texture2D   u_Layer0Normal ("Layer 0 Normal", Category("Layer 0"), Intent(NormalMap), Element(u_LayerNormal, 0))
        Texture2D   u_Layer0ARM ("Layer 0 ARM", Category("Layer 0"), Intent(Data), Element(u_LayerARM, 0))
        Texture2D   u_Layer0Height ("Layer 0 Height", Category("Layer 0"), Intent(Data), Element(u_LayerHeight, 0))
        LandscapeLayerInfo Layer1Layer ("Layer 1 Layer", Category("Layer 1"), Element(Layers, 1))
        Int         Layer1Blend ("Layer 1 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 1")) = 0
        Float       Layer1TileSize ("Layer 1 Tile Size (cm)", Range(10,10000), Category("Layer 1")) = 300
        Color       Layer1Tint ("Layer 1 Tint", Category("Layer 1")) = (1, 1, 1, 1)
        Texture2D   u_Layer1Albedo ("Layer 1 Albedo", Category("Layer 1"), Intent(Colour), Element(u_LayerAlbedo, 1))
        Texture2D   u_Layer1Normal ("Layer 1 Normal", Category("Layer 1"), Intent(NormalMap), Element(u_LayerNormal, 1))
        Texture2D   u_Layer1ARM ("Layer 1 ARM", Category("Layer 1"), Intent(Data), Element(u_LayerARM, 1))
        Texture2D   u_Layer1Height ("Layer 1 Height", Category("Layer 1"), Intent(Data), Element(u_LayerHeight, 1))
        LandscapeLayerInfo Layer2Layer ("Layer 2 Layer", Category("Layer 2"), Element(Layers, 2))
        Int         Layer2Blend ("Layer 2 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 2")) = 0
        Float       Layer2TileSize ("Layer 2 Tile Size (cm)", Range(10,10000), Category("Layer 2")) = 300
        Color       Layer2Tint ("Layer 2 Tint", Category("Layer 2")) = (1, 1, 1, 1)
        Texture2D   u_Layer2Albedo ("Layer 2 Albedo", Category("Layer 2"), Intent(Colour), Element(u_LayerAlbedo, 2))
        Texture2D   u_Layer2Normal ("Layer 2 Normal", Category("Layer 2"), Intent(NormalMap), Element(u_LayerNormal, 2))
        Texture2D   u_Layer2ARM ("Layer 2 ARM", Category("Layer 2"), Intent(Data), Element(u_LayerARM, 2))
        Texture2D   u_Layer2Height ("Layer 2 Height", Category("Layer 2"), Intent(Data), Element(u_LayerHeight, 2))
        LandscapeLayerInfo Layer3Layer ("Layer 3 Layer", Category("Layer 3"), Element(Layers, 3))
        Int         Layer3Blend ("Layer 3 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 3")) = 0
        Float       Layer3TileSize ("Layer 3 Tile Size (cm)", Range(10,10000), Category("Layer 3")) = 300
        Color       Layer3Tint ("Layer 3 Tint", Category("Layer 3")) = (1, 1, 1, 1)
        Texture2D   u_Layer3Albedo ("Layer 3 Albedo", Category("Layer 3"), Intent(Colour), Element(u_LayerAlbedo, 3))
        Texture2D   u_Layer3Normal ("Layer 3 Normal", Category("Layer 3"), Intent(NormalMap), Element(u_LayerNormal, 3))
        Texture2D   u_Layer3ARM ("Layer 3 ARM", Category("Layer 3"), Intent(Data), Element(u_LayerARM, 3))
        Texture2D   u_Layer3Height ("Layer 3 Height", Category("Layer 3"), Intent(Data), Element(u_LayerHeight, 3))
        LandscapeLayerInfo Layer4Layer ("Layer 4 Layer", Category("Layer 4"), Element(Layers, 4))
        Int         Layer4Blend ("Layer 4 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 4")) = 0
        Float       Layer4TileSize ("Layer 4 Tile Size (cm)", Range(10,10000), Category("Layer 4")) = 300
        Color       Layer4Tint ("Layer 4 Tint", Category("Layer 4")) = (1, 1, 1, 1)
        Texture2D   u_Layer4Albedo ("Layer 4 Albedo", Category("Layer 4"), Intent(Colour), Element(u_LayerAlbedo, 4))
        Texture2D   u_Layer4Normal ("Layer 4 Normal", Category("Layer 4"), Intent(NormalMap), Element(u_LayerNormal, 4))
        Texture2D   u_Layer4ARM ("Layer 4 ARM", Category("Layer 4"), Intent(Data), Element(u_LayerARM, 4))
        Texture2D   u_Layer4Height ("Layer 4 Height", Category("Layer 4"), Intent(Data), Element(u_LayerHeight, 4))
        LandscapeLayerInfo Layer5Layer ("Layer 5 Layer", Category("Layer 5"), Element(Layers, 5))
        Int         Layer5Blend ("Layer 5 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 5")) = 0
        Float       Layer5TileSize ("Layer 5 Tile Size (cm)", Range(10,10000), Category("Layer 5")) = 300
        Color       Layer5Tint ("Layer 5 Tint", Category("Layer 5")) = (1, 1, 1, 1)
        Texture2D   u_Layer5Albedo ("Layer 5 Albedo", Category("Layer 5"), Intent(Colour), Element(u_LayerAlbedo, 5))
        Texture2D   u_Layer5Normal ("Layer 5 Normal", Category("Layer 5"), Intent(NormalMap), Element(u_LayerNormal, 5))
        Texture2D   u_Layer5ARM ("Layer 5 ARM", Category("Layer 5"), Intent(Data), Element(u_LayerARM, 5))
        Texture2D   u_Layer5Height ("Layer 5 Height", Category("Layer 5"), Intent(Data), Element(u_LayerHeight, 5))
        LandscapeLayerInfo Layer6Layer ("Layer 6 Layer", Category("Layer 6"), Element(Layers, 6))
        Int         Layer6Blend ("Layer 6 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 6")) = 0
        Float       Layer6TileSize ("Layer 6 Tile Size (cm)", Range(10,10000), Category("Layer 6")) = 300
        Color       Layer6Tint ("Layer 6 Tint", Category("Layer 6")) = (1, 1, 1, 1)
        Texture2D   u_Layer6Albedo ("Layer 6 Albedo", Category("Layer 6"), Intent(Colour), Element(u_LayerAlbedo, 6))
        Texture2D   u_Layer6Normal ("Layer 6 Normal", Category("Layer 6"), Intent(NormalMap), Element(u_LayerNormal, 6))
        Texture2D   u_Layer6ARM ("Layer 6 ARM", Category("Layer 6"), Intent(Data), Element(u_LayerARM, 6))
        Texture2D   u_Layer6Height ("Layer 6 Height", Category("Layer 6"), Intent(Data), Element(u_LayerHeight, 6))
        LandscapeLayerInfo Layer7Layer ("Layer 7 Layer", Category("Layer 7"), Element(Layers, 7))
        Int         Layer7Blend ("Layer 7 Blend (0 Weight, 1 Height, 2 Alpha)", Range(0,2), Category("Layer 7")) = 0
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

    // ── The split every stage below repeats ─────────────────────────────────────────────────────────
    //
    // TerrainUB (binding 0) holds ONLY what every terrain of a frame shares: View, Projection and the
    // sun. Everything per-tile — its frame, sizes, layer modes — is a row of TerrainInstances[]
    // (binding 8), named per draw by the SAME push-constant index that names the material param row.
    //
    // WHY: the terrain pass records every draw of a frame before the GPU executes any of them, and a
    // material's uniform buffer has one copy per frame — so per-terrain fields kept in TerrainUB were
    // read as the LAST terrain's values by every terrain (two terrains drew as one). A push constant is
    // snapshotted at record time, so a row named by it cannot be clobbered by the next draw's setup —
    // the exact argument that already moved the material params to Materials[] (MaterialParamRow.hpp).
    // The C++ mirror of TerrainInstance lives in Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp,
    // and the ShaderCacheKey suite asserts both statements of the layout against the compiled SPIR-V.

    // The vertex stage is TerrainVertex.glslh, shared with TerrainGBuffer.shader and TerrainShadow.shader:
    // one statement of the grid, its LOD and its displacement, so the surface the cascades see is the
    // surface the camera sees. The vertex stage projects with the push-constant
    // matrix (m_PushConstants.Transform): the view's JitteredViewProjection here (raster geometry is jittered for TAA), the cascade's matrix in the shadow
    // pass — per draw, snapshotted at record, so one material serves every cascade.
    //
    // THIS PROGRAM IS THE FORWARD RENDER PATH'S. In Deferred the terrain writes the G-buffer instead
    // (TerrainGBuffer.shader) and this pass does not draw, so there is one lighting of the ground per path.

    Vertex
    {
        #include <Programs/Terrain/TerrainVertex.glslh>
    }

    Fragment
    {
        Out(0) vec4 o_Color;
        Out(1) vec2 oVelocity; // the view's velocity, slot 1 of the scene target (TerrainSurface.glslh)

        #include <Programs/Terrain/TerrainSurface.glslh>

        // THE CLOUD LAYER'S SHADOW ON THE WORLD. The terrain is the surface this feature is FOR — a deck
        // of cumulus drifting over open ground is the whole picture — and it is the surface that never
        // received it: the wrapper lived inside the deferred composite, and a terrain is drawn by neither
        // render path's mesh shaders. Slots 6/7 are the first free ones in this shader's own small layout;
        // they need not (and do not) match the mesh shaders' 20/21, because only the NAMES are shared and
        // every material binds by name (Graphic::CloudShadowBind).
        Uniform(6) sampler2D u_CloudShadowMap;
        Uniform(7) CloudShadowUB
        {
        	mat4 u_CloudShadowWorldToMap;
        	// x = the kilometres the map's clip z spans, y = 1 when the map is real and must be read,
        	// z = the UV width of the border fade, w = the artist's shadow strength.
        	vec4 u_CloudShadowParams;
        };

        // THE receiver, shared verbatim with the deferred composite and the mesh shaders.
        #include <Common/CloudShadowReceiver.glslh>
        // FORWARD ONLY: in the Deferred path the composite applies it to the terrain's G-buffer texels.

        void main()
        {
            TerrainSurface s = EvaluateTerrainSurface();
            vec3 N           = s.N;
            vec3 albedo      = s.Albedo;

            // Diffuse lighting using the SCENE directional light (dir/color/intensity). The ground is matte:
            // no layer carries a specular term.
            vec3 L      = normalize( -u.SunDir.xyz );          // to-light (engine stores travel direction)
            vec3 sun    = u.SunColor.rgb * max( u.SunColor.a, 0.0001 );

            float ndl = max( dot( N, L ), 0.0 );

            // Soft sky/ground ambient (hemisphere): brighter from above, cooler/darker from below.
            vec3  skyCol  = vec3( 0.45, 0.52, 0.62 );
            vec3  gndCol  = vec3( 0.20, 0.18, 0.14 );
            vec3  ambient = mix( gndCol, skyCol, N.y * 0.5 + 0.5 ) * 0.5;

            // The cloud layer attenuates the SUN and nothing else: the hemisphere ambient above is the
            // whole sky dome, which a deck occludes with a different geometry than a direction. Same
            // split the deferred composite and the mesh shaders make.
            float cloudShadow = CloudShadowFactor( v_WorldPos );

            vec3 lit = albedo * ( ambient * s.AO + sun * ndl * cloudShadow );

            o_Color   = vec4( lit * u_Material.Tint.rgb, 1.0 );
            oVelocity = TerrainVelocity();
        }
    }
}
