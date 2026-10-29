// DesertAsset {"Kind":"Shader","Guid":"acb8d06bbe1a49dc95c2f5ebdddb4c33","Versions":{"SHDR":1},"Dependencies":[]}
Shader "FoliageSurface"
{
    // The foliage surface template: StandardSurface's surface (the same maps, the same Properties, the same Import
    // rows) lit by the TwoSidedFoliage shading model (ShadingModels/TwoSidedFoliage.shadingmodel, UE's
    // MSM_TwoSidedFoliage) instead of DefaultLit, with the model's payload — SubsurfaceColor — written into
    // CustomData0..2. A leaf card is a thin two-sided sheet the sun shines through; lit as an opaque one-sided
    // surface, its side turned from the sun took only the far sky's blue (Bistro's cypresses and hedges).

    Domain Surface

    // THE IMPORT CONTRACT (see StandardSurface). This template takes a source material only when the source
    // states it is BOTH two-sided and alpha-masked — a cut-out card seen from either side, which is what a leaf,
    // a frond or a flower is in a real-time scene — and then over StandardSurface, because it requires more.
    // FBX states the first by assimp's AI_MATKEY_TWOSIDED or the ORCA ".DoubleSided" name suffix and the second
    // by the albedo's cut-out alpha (Assimp/SourceMaterialAdapter.cpp). glTF's statement of foliage is
    // KHR_materials_diffuse_transmission, which the adapter does not read yet.
    Import
    {
        Requires "fbx.doubleSided"
        Requires "fbx.alphaMask"
        "gltf.baseColorFactor"          -> AlbedoColor
        "gltf.baseColorTexture"         -> AlbedoTexture
        "gltf.metallicFactor"           -> MetallicFactor
        "gltf.roughnessFactor"          -> RoughnessFactor
        "gltf.metallicRoughnessTexture" -> OrmTexture.gb
        "gltf.normalTexture"            -> NormalTexture
        "gltf.occlusionTexture"         -> OrmTexture.r
        "gltf.occlusionStrength"        -> AOStrength
        "gltf.normalScale"              -> NormalScale
        "gltf.emissiveFactor"           -> EmissiveColor
        "gltf.emissiveTexture"          -> EmissiveTexture
        "gltf.emissiveStrength"         -> EmissiveIntensity
        "gltf.alphaCutoff"              -> OpacityMaskClipValue
        "gltf.alphaMask"                -> OpacityTexture
        "gltf.alphaMask"                -> OpacityChannel
        "gltf.uvOffset"                 -> UVOffset
        "gltf.uvScale"                  -> UVTiling
        "gltf.uvRotation"               -> UVRotation
        "fbx.DiffuseColor"              -> AlbedoColor
        "fbx.DiffuseColor"              -> AlbedoTexture
        "fbx.NormalMap"                 -> NormalTexture
        "fbx.EmissiveColor"             -> EmissiveColor
        "fbx.EmissiveColor"             -> EmissiveTexture
        "fbx.TransparentColor"          -> OpacityTexture
        "fbx.OcclusionRoughnessMetallic" -> MetallicFactor
        "fbx.RoughnessMetallic"         -> MetallicFactor
        "fbx.Metalness"                 -> MetallicFactor
        "fbx.Metalness"                 -> OrmTexture.b
        "fbx.OcclusionRoughnessMetallic" -> RoughnessFactor
        "fbx.RoughnessMetallic"         -> RoughnessFactor
        "fbx.Roughness"                 -> RoughnessFactor
        "fbx.Roughness"                 -> OrmTexture.g
        "fbx.AmbientOcclusion"          -> OrmTexture.r
        "fbx.OcclusionRoughnessMetallic" -> OrmTexture.rgb
        "fbx.RoughnessMetallic"         -> OrmTexture.gb
        "fbx.alphaCutoff"               -> OpacityMaskClipValue
        "fbx.alphaMask"                 -> OpacityTexture
        "fbx.alphaMask"                 -> OpacityChannel
    }

    // ONE parameter layout for every cell (forward, instanced, GBuffer, skinned): StandardSurface's, plus the
    // subsurface colour.
    Properties Binding(2)
    {
        Color       AlbedoColor ("Albedo", Category("Surface")) = (1, 1, 1, 1)
        Float       MetallicFactor ("Metallic", Range(0,1), Category("Surface")) = 0
        Float       RoughnessFactor ("Roughness", Range(0,1), Category("Surface")) = 0.5
        // UE's Specular pin: the dielectric reflectance, F0 = 0.08 x Specular. 0.5 (UE's default) is the 4 % of a
        // common dielectric; a leaf whose sheen reads milky over a dark albedo takes less (UE foliage practice).
        Float       Specular ("Specular", Range(0,1), Category("Surface")) = 0.5
        // UE's Ambient Occlusion input, ONE home (StandardSurface, MAT-AO-ONEHOME): the strength of the ORM map's R
        // (glTF occlusionStrength), applied once in SurfaceResolveORM. The template had copied the retired pair
        // (AOStrength multiplying, OcclusionStrength lerping the map), so one authored 0.5 was applied as 0.25.
        Float       AOStrength ("Ambient Occlusion", Range(0,1), Category("Surface")) = 1
        Color       EmissiveColor ("Emissive", Category("Surface")) = (0, 0, 0, 1)
        Float       EmissiveIntensity ("Emissive Intensity", Range(0,100), Category("Surface")) = 1
        // UE's Opacity Mask Clip Value (default 0.3333): a texel whose mask is below it is cut in every pass.
        Float       OpacityMaskClipValue ("Opacity Mask Clip Value", Range(0,1), Category("Surface")) = 0.3333
        Vec2        UVTiling ("UV Tiling", Category("Surface")) = (1, 1)
        Vec2        UVOffset ("UV Offset", Category("Surface")) = (0, 0)
        Float       UVRotation ("UV Rotation", Range(-3.14159,3.14159), Category("Surface")) = 0
        Float       NormalScale ("Normal Scale", Range(0,4), Category("Surface")) = 1
        // UE's Dither Opacity Mask: 1 = keep a pixel with probability = mask (TAA resolves it) instead of cutting at
        // the clip value. For leaves smaller than a texel of the mip they sample (a crown of single-leaf cards seen from
        // afar): no threshold on a mip can keep each leaf's share, a dither can. Its opacity texture then keeps the
        // plain mean in its mips (no AlphaCoverageThresholds).
        Float       DitherOpacityMask ("Dither Opacity Mask", Range(0,1), Category("Surface")) = 0
        // Which channel of OpacityTexture is the mask: 0 = R of a separate opacity map, 3 = A (the importer binds the
        // albedo texture itself there for a glTF MASK). Stated, never guessed from the bound texture's size.
        Float       OpacityChannel ("Opacity Channel", Range(0,3), Category("Surface")) = 0
        // Material half of the sun-shadow receive decision; the renderer also zeroes it for a mesh whose
        // Receive Shadows toggle is off, so a surface skips the sun shadow when EITHER says so.
        Float       ReceiveSunShadows ("Receive Sun Shadows", Range(0,1), Category("Shadows")) = 1
        // The light a leaf lets through, as a tint of its own albedo: SubsurfaceColor = albedo * this (white = the
        // leaf transmits the colour it reflects). UE's SubsurfaceColor pin; the model reads it from the payload.
        Color       SubsurfaceColor ("Subsurface Color", Category("Foliage")) = (1, 1, 1, 1)
        Texture2D   AlbedoTexture ("Albedo Map", Category("Textures"), Intent(Colour))
        // The ONE slot whose empty state is not white. A normal map is unpacked with `2*t - 1`, so a
        // white texel decodes to a normalised (1,1,1) — a normal tilted 54 degrees off the surface —
        // whereas (0.5,0.5,1) decodes to +Z, which is what "this surface has no normal detail" means.
        // The fragment stages here, in StaticMeshGBuffer and in StaticMeshLit_Instanced all guard with
        // `textureSize(NormalTexture,0).x > 1` and skip a 1x1, so this changes no pixel today; it is
        // written down so the guard is a fast path rather than the only thing standing between an empty
        // slot and a wrong normal.
        Texture2D   NormalTexture ("Normal Map", Category("Textures"), Intent(NormalMap)) = "normal"
        Texture2D   OpacityTexture ("Opacity Map", Category("Textures"), Intent(Mask))
        // Packed glTF-style: R = occlusion, G = roughness, B = metallic, each multiplying its factor; white when empty.
        Texture2D   OrmTexture ("ORM Map", Category("Textures"), Intent(PackedMasks))
        Texture2D   EmissiveTexture ("Emissive Map", Category("Textures"), Intent(Colour))
    }

    ShadingModel TwoSidedFoliage
    // A leaf card is cut out (UE: foliage materials are BlendMode Masked): every cell clips with the generated mask,
    // and the card casts through its own ShadowDepth cell in the cascades and the local lights' atlas.
    BlendMode Masked
    OpacityMask OpacityTexture Channel(OpacityChannel) UV(0) Transform(UVTiling, UVOffset, UVRotation)

    // The surface maps: the template declares its own samplers (the cells add no material textures).
    Surface
    {

        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s = DefaultSurfaceOutput();
            vec2 tiling = Material.UVTiling;
            if ( tiling.x <= 0.0 ) tiling.x = 1.0;
            if ( tiling.y <= 0.0 ) tiling.y = 1.0;
            const vec2 uv = SurfaceTransformUV( SurfaceSelectUV( i.UV0, i.UV0, 0 ), Material.UVOffset, tiling,
                                            Material.UVRotation );
            s.BaseColor = SurfaceBaseColor( Material.AlbedoColor.rgb,
                                        SurfaceSampleMaterial( Material.AlbedoTexture, uv ).rgb, vec3( 1.0 ) );
            const ivec2 normalSize = BindlessTextureSize( Material.NormalTexture, 0 );
            if ( normalSize.x > 1 && normalSize.y > 1 )
                s.Normal = SurfaceScaleTangentNormal( SurfaceSampleTangentNormal( Material.NormalTexture, uv ), Material.NormalScale );
            const vec3 orm = SurfaceResolveORM( SurfaceSampleMaterial( Material.OrmTexture, uv ).rgb, Material.AOStrength,
                                            Material.RoughnessFactor, Material.MetallicFactor );
            s.Metallic          = orm.z;
            s.Roughness         = orm.y;
            s.Specular          = Material.Specular;
            s.AmbientOcclusion  = orm.x;
            s.Emissive          = SurfaceEmission( SurfaceSampleMaterial( Material.EmissiveTexture, uv ).rgb,
                                               Material.EmissiveColor.rgb, Material.EmissiveIntensity );
            // The renderer zeroes this row field for objects that must not take the sun's shadow.
            s.ReceiveSunShadows = Material.ReceiveSunShadows;
            // TwoSidedFoliage's payload: SubsurfaceColor stored as its square root (UE EncodeSubsurfaceColor; the
            // model squares it back), clamped to the payload's [0, 1].
            const vec3 subsurface = sqrt( clamp( s.BaseColor * Material.SubsurfaceColor.rgb, 0.0, 1.0 ) );
            s.CustomData0       = subsurface.r;
            s.CustomData1       = subsurface.g;
            s.CustomData2       = subsurface.b;
            // UE CustomData.a = the material's Opacity, the leaf's subsurface density for the contact shadows (the
            // mask above discards; what survives is opaque, UE's Opacity default 1).
            s.CustomData3       = s.Opacity;
            s.SampledTextureCount = float( int( BindlessTextureSize( Material.AlbedoTexture, 0 ).x > 1 ) + int( normalSize.x > 1 ) +
                                           int( BindlessTextureSize( Material.OpacityTexture, 0 ).x > 1 ) +
                                           int( BindlessTextureSize( Material.OrmTexture, 0 ).x > 1 ) +
                                           int( BindlessTextureSize( Material.EmissiveTexture, 0 ).x > 1 ) );
            return s;
        }
    }
}
