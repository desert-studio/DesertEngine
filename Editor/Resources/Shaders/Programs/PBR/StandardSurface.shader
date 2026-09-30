// DesertAsset {"Kind":"Shader","Guid":"4f1cac6af403a010c792d835dd6f7d44","Versions":{"SHDR":1},"Dependencies":[]}
Shader "StandardSurface"
{
    // The standard surface template. Like any `Domain Surface` shader it is a Properties block plus ONE
    // surface function; DShaderParser expands it into the (Static|Instanced|Skinned) x
    // (Forward|GBuffer|ShadowDepth) cells, each the engine's Mesh/Surface vertex-path and pass headers
    // around EvaluateSurface below. MeshShaderFor (Materials/Mesh/MeshVertexPath.cpp) names the cell a
    // mesh pass draws with, as "StandardSurface/<Path>.<Pass>". Its identity is the GUID above, which it
    // kept from StaticMeshPBR.shader (the five hand-written PBR programs it replaces); the name is a label.
    //
    // The Properties below are the SINGLE source of truth for what a PBR material stores: every .demat
    // persists them as generic ShaderParams/ShaderTextures, the same protocol as any DSL shader.

    Domain Surface
    Role PBRSurface
    Default Surface

    // THE IMPORT CONTRACT (MAT1 adapters): the importer carries a source material as the source's own
    // dictionary and never names a template; this block says which source keys feed which Property here
    // (a texture row may name the source channels it takes, glTF packing roughness in G and metal in B).
    // A key may be both a value and a texture (FBX binds maps to its colour properties): the Property's
    // kind says which half it takes. The DSL parser refuses a row whose Property is not declared below.
    Import
    {
        "gltf.baseColorFactor"          -> AlbedoColor
        "gltf.baseColorTexture"         -> u_AlbedoTexture
        "gltf.metallicFactor"           -> MetallicFactor
        "gltf.roughnessFactor"          -> RoughnessFactor
        "gltf.metallicRoughnessTexture" -> u_ORMTexture.gb
        "gltf.normalTexture"            -> u_NormalTexture
        "gltf.occlusionTexture"         -> u_ORMTexture.r
        "gltf.occlusionStrength"        -> OcclusionStrength
        "gltf.normalScale"              -> NormalScale
        "gltf.emissiveFactor"           -> EmissiveColor
        "gltf.emissiveTexture"          -> u_EmissiveTexture
        "gltf.emissiveStrength"         -> EmissiveIntensity
        "gltf.alphaCutoff"              -> AlphaCutoff
        "gltf.alphaMask"                -> u_OpacityTexture
        "gltf.alphaMask"                -> OpacityChannel
        "gltf.uvOffset"                 -> UVOffset
        "gltf.uvScale"                  -> UVTiling
        "gltf.uvRotation"               -> UVRotation
        "fbx.DiffuseColor"              -> AlbedoColor
        "fbx.DiffuseColor"              -> u_AlbedoTexture
        "fbx.NormalMap"                 -> u_NormalTexture
        "fbx.EmissiveColor"             -> EmissiveColor
        "fbx.EmissiveColor"             -> u_EmissiveTexture
        "fbx.TransparentColor"          -> u_OpacityTexture
        "fbx.Metalness"                 -> MetallicFactor
        "fbx.Metalness"                 -> u_ORMTexture.b
        "fbx.Roughness"                 -> RoughnessFactor
        "fbx.Roughness"                 -> u_ORMTexture.g
        "fbx.AmbientOcclusion"          -> u_ORMTexture.r
        "fbx.alphaCutoff"               -> AlphaCutoff
    }

    // ONE parameter layout for every cell (forward, instanced, GBuffer, skinned): the cells share this one
    // Properties block. Glass is its own template (StaticMeshGlass, BlendMode Translucent) with its own row.
    Properties Binding(2)
    {
        Color       AlbedoColor ("Albedo", Category("Surface")) = (1, 1, 1, 1)
        Float       MetallicFactor ("Metallic", Range(0,1), Category("Surface")) = 0
        Float       RoughnessFactor ("Roughness", Range(0,1), Category("Surface")) = 0.5
        Float       AOStrength ("Ambient Occlusion", Range(0,1), Category("Surface")) = 1
        Color       EmissiveColor ("Emissive", Category("Surface")) = (0, 0, 0, 1)
        Float       EmissiveIntensity ("Emissive Intensity", Range(0,100), Category("Surface")) = 1
        Float       AlphaCutoff ("Alpha Cutoff", Range(0,1), Category("Surface")) = 0
        Vec2        UVTiling ("UV Tiling", Category("Surface")) = (1, 1)
        Vec2        UVOffset ("UV Offset", Category("Surface")) = (0, 0)
        Float       UVRotation ("UV Rotation", Range(-3.14159,3.14159), Category("Surface")) = 0
        Float       NormalScale ("Normal Scale", Range(0,4), Category("Surface")) = 1
        Float       OcclusionStrength ("Occlusion Strength", Range(0,1), Category("Surface")) = 1
        // Which channel of u_OpacityTexture is the mask: 0 = R of a separate opacity map, 3 = A (the importer binds the
        // albedo texture itself there for a glTF MASK). Stated, never guessed from the bound texture's size.
        Float       OpacityChannel ("Opacity Channel", Range(0,3), Category("Surface")) = 0
        // Material half of the sun-shadow receive decision; the renderer also zeroes it for a mesh whose
        // Receive Shadows toggle is off, so a surface skips the sun shadow when EITHER says so.
        Float       ReceiveSunShadows ("Receive Sun Shadows", Range(0,1), Category("Shadows")) = 1
        Texture2D   u_AlbedoTexture ("Albedo Map", Category("Textures"))
        // The ONE slot whose empty state is not white. A normal map is unpacked with `2*t - 1`, so a
        // white texel decodes to a normalised (1,1,1) — a normal tilted 54 degrees off the surface —
        // whereas (0.5,0.5,1) decodes to +Z, which is what "this surface has no normal detail" means.
        // The fragment stages here, in StaticMeshGBuffer and in StaticMeshPBR_Instanced all guard with
        // `textureSize(u_NormalTexture,0).x > 1` and skip a 1x1, so this changes no pixel today; it is
        // written down so the guard is a fast path rather than the only thing standing between an empty
        // slot and a wrong normal.
        Texture2D   u_NormalTexture ("Normal Map", Category("Textures")) = "normal"
        Texture2D   u_OpacityTexture ("Opacity Map", Category("Textures"))
        // Packed glTF-style: R = occlusion, G = roughness, B = metallic, each multiplying its factor; white when empty.
        Texture2D   u_ORMTexture ("ORM Map", Category("Textures"))
        Texture2D   u_EmissiveTexture ("Emissive Map", Category("Textures"))
    }

    // The surface maps: the template declares its own samplers (the cells add no material textures).
    Surface
    {
        layout( binding = 11 ) uniform sampler2D u_AlbedoTexture;
        layout( binding = 12 ) uniform sampler2D u_NormalTexture;
        layout( binding = 18 ) uniform sampler2D u_OpacityTexture;
        layout( binding = 23 ) uniform sampler2D u_ORMTexture;
        layout( binding = 24 ) uniform sampler2D u_EmissiveTexture;

        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s = DefaultSurfaceOutput();
            vec2 tiling = u_Material.UVTiling;
            if ( tiling.x <= 0.0 ) tiling.x = 1.0;
            if ( tiling.y <= 0.0 ) tiling.y = 1.0;
            const vec2 uv = PBRTransformUV( PBRSelectUV( i.UV0, i.UV0, 0 ), u_Material.UVOffset, tiling,
                                            u_Material.UVRotation );
            const float mask = texture( u_OpacityTexture, uv )[int( u_Material.OpacityChannel )];
            if ( u_Material.AlphaCutoff > 0.0 && mask < u_Material.AlphaCutoff )
                discard;
            s.BaseColor = PBRBaseColor( u_Material.AlbedoColor.rgb,
                                        pow( texture( u_AlbedoTexture, uv ).rgb, vec3( 2.2 ) ), vec3( 1.0 ) );
            const ivec2 normalSize = textureSize( u_NormalTexture, 0 );
            if ( normalSize.x > 1 && normalSize.y > 1 )
                s.Normal = PBRScaleTangentNormal( SampleTangentNormal( u_NormalTexture, uv ), u_Material.NormalScale );
            const vec3 orm = PBRResolveORM( texture( u_ORMTexture, uv ).rgb, u_Material.OcclusionStrength,
                                            u_Material.RoughnessFactor, u_Material.MetallicFactor );
            s.Metallic          = orm.z;
            s.Roughness         = orm.y;
            s.AmbientOcclusion  = u_Material.AOStrength * orm.x;
            s.Emissive          = PBREmission( pow( texture( u_EmissiveTexture, uv ).rgb, vec3( 2.2 ) ),
                                               u_Material.EmissiveColor.rgb, u_Material.EmissiveIntensity );
            // The renderer zeroes this row field for objects that must not take the sun's shadow.
            s.ReceiveSunShadows = u_Material.ReceiveSunShadows;
            s.SampledTextureCount = float( int( textureSize( u_AlbedoTexture, 0 ).x > 1 ) + int( normalSize.x > 1 ) +
                                           int( textureSize( u_OpacityTexture, 0 ).x > 1 ) +
                                           int( textureSize( u_ORMTexture, 0 ).x > 1 ) +
                                           int( textureSize( u_EmissiveTexture, 0 ).x > 1 ) );
            return s;
        }
    }
}
