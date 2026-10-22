// DesertAsset {"Kind":"Shader","Guid":"7b1d37900d0349d284ba0faa36a13dc7","Versions":{"SHDR":1},"Dependencies":[]}
Shader "StandardSurfaceMasked"
{
    // The standard surface, CUT OUT (UE: a DefaultLit material with BlendMode Masked): StandardSurface's surface, maps
    // and rows, plus the opacity mask its `OpacityMask` line states and the OpacityMaskClipValue threshold. The cells
    // clip with the parser's generated DesertSurfaceOpacityMask() in every pass (Forward, GBuffer, ShadowDepth), and
    // a Masked material casts through its own ShadowDepth cell (MeshRenderer::MaskedCasterMaterial) in the cascades
    // and the local lights' atlas. Ray tracing reads the same statement from ShaderProgramMeta::OpacityMask.

    Domain Surface
    Role StandardSurfaceMasked

    // THE IMPORT CONTRACT (MAT1 adapters): the importer carries a source material as the source's own
    // dictionary and never names a template; this block says which source keys feed which Property here
    // (a texture row may name the source channels it takes, glTF packing roughness in G and metal in B).
    // A key may be both a value and a texture (FBX binds maps to its colour properties): the Property's
    // kind says which half it takes. The DSL parser refuses a row whose Property is not declared below.
    Import
    {
        // Taken by every source the adapter states cut out (SourceMaterialAdapter: "source.alphaMask", one key for
        // glTF MASK and an FBX whose albedo alpha cuts out); FoliageSurface requires more and wins the two-sided ones.
        Requires "source.alphaMask"
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

    // ONE parameter layout for every cell (forward, instanced, GBuffer, skinned): the cells share this one
    // Properties block. Glass is its own template (StaticMeshGlass, BlendMode Translucent) with its own row.
    Properties Binding(2)
    {
        Color       AlbedoColor ("Albedo", Category("Surface")) = (1, 1, 1, 1)
        Float       MetallicFactor ("Metallic", Range(0,1), Category("Surface")) = 0
        Float       RoughnessFactor ("Roughness", Range(0,1), Category("Surface")) = 0.5
        // UE's Ambient Occlusion input, ONE home: the strength of the ORM map's R (glTF occlusionStrength), applied
        // once in SurfaceResolveORM — 1 takes the map as is, 0 ignores it.
        Float       AOStrength ("Ambient Occlusion", Range(0,1), Category("Surface")) = 1
        Color       EmissiveColor ("Emissive", Category("Surface")) = (0, 0, 0, 1)
        Float       EmissiveIntensity ("Emissive Intensity", Range(0,100), Category("Surface")) = 1
        // UE's Opacity Mask Clip Value (default 0.3333): a texel whose mask is below it is cut in every pass.
        Float       OpacityMaskClipValue ("Opacity Mask Clip Value", Range(0,1), Category("Surface")) = 0.3333
        Vec2        UVTiling ("UV Tiling", Category("Surface")) = (1, 1)
        Vec2        UVOffset ("UV Offset", Category("Surface")) = (0, 0)
        Float       UVRotation ("UV Rotation", Range(-3.14159,3.14159), Category("Surface")) = 0
        Float       NormalScale ("Normal Scale", Range(0,4), Category("Surface")) = 1
        // Which channel of OpacityTexture is the mask: 0 = R of a separate opacity map, 3 = A (the importer binds the
        // albedo texture itself there for a glTF MASK). Stated, never guessed from the bound texture's size.
        Float       OpacityChannel ("Opacity Channel", Range(0,3), Category("Surface")) = 0
        // Material half of the sun-shadow receive decision; the renderer also zeroes it for a mesh whose
        // Receive Shadows toggle is off, so a surface skips the sun shadow when EITHER says so.
        Float       ReceiveSunShadows ("Receive Sun Shadows", Range(0,1), Category("Shadows")) = 1
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

    BlendMode Masked
    // UE's OpacityMask input: the Opacity Map's stated channel at UV0 through the surface's UV transform.
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
            s.AmbientOcclusion  = orm.x;
            s.Emissive          = SurfaceEmission( SurfaceSampleMaterial( Material.EmissiveTexture, uv ).rgb,
                                               Material.EmissiveColor.rgb, Material.EmissiveIntensity );
            // The renderer zeroes this row field for objects that must not take the sun's shadow.
            s.ReceiveSunShadows = Material.ReceiveSunShadows;
            s.SampledTextureCount = float( int( BindlessTextureSize( Material.AlbedoTexture, 0 ).x > 1 ) + int( normalSize.x > 1 ) +
                                           int( BindlessTextureSize( Material.OpacityTexture, 0 ).x > 1 ) +
                                           int( BindlessTextureSize( Material.OrmTexture, 0 ).x > 1 ) +
                                           int( BindlessTextureSize( Material.EmissiveTexture, 0 ).x > 1 ) );
            return s;
        }
    }
}
