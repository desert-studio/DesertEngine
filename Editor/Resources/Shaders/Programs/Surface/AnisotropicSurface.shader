// DesertAsset {"Kind":"Shader","Guid":"f084976b1657f6846a2334d69621be15","Versions":{"SHDR":1},"Dependencies":[]}
// The anisotropic surface template (UE: a Default Lit material with the Anisotropy and Tangent pins):
// StandardSurface's maps lit by the Anisotropic shading model (ShadingModels/Anisotropic.shadingmodel) - brushed
// metal, satin, machined discs. The stretch direction is the mesh tangent (UV0's U axis), turned by the Anisotropy
// Direction map (tangent space RG, decoded as a normal map's XY; a 1x1 slot keeps the mesh tangent - circular
// brushing is a map) and then by Anisotropy Rotation about the normal. The template writes the payload:
// CustomData0 = Anisotropy * 0.5 + 0.5, CustomData1 = the world tangent's angle about the shading normal
// (Common/AnisotropicTangent.glslh). It declares no Import block: the importer never picks it, a material does.
Shader "AnisotropicSurface"
{
    Domain Surface

    // ONE parameter layout for every cell (forward, instanced, GBuffer, skinned): StandardSurface's maps and
    // factors, plus the model's payload rows.
    Properties Binding(2)
    {
        Color       AlbedoColor ("Albedo", Category("Surface")) = (1, 1, 1, 1)
        Float       MetallicFactor ("Metallic", Range(0,1), Category("Surface")) = 0
        Float       RoughnessFactor ("Roughness", Range(0,1), Category("Surface")) = 0.5
        Float       AOStrength ("Ambient Occlusion", Range(0,1), Category("Surface")) = 1
        Color       EmissiveColor ("Emissive", Category("Surface")) = (0, 0, 0, 1)
        Float       EmissiveIntensity ("Emissive Intensity", Range(0,100), Category("Surface")) = 1
        Vec2        UVTiling ("UV Tiling", Category("Surface")) = (1, 1)
        Vec2        UVOffset ("UV Offset", Category("Surface")) = (0, 0)
        Float       UVRotation ("UV Rotation", Range(-3.14159,3.14159), Category("Surface")) = 0
        Float       NormalScale ("Normal Scale", Range(0,4), Category("Surface")) = 1
        // Material half of the sun-shadow receive decision; the renderer also zeroes it for a mesh whose
        // Receive Shadows toggle is off, so a surface skips the sun shadow when EITHER says so.
        Float       ReceiveSunShadows ("Receive Sun Shadows", Range(0,1), Category("Shadows")) = 1
        // The Anisotropic model's payload (ShadingModels/Anisotropic.shadingmodel): -1..1, > 0 stretches the
        // highlight along the tangent, < 0 across it, 0 is DefaultLit's lobe.
        Float       Anisotropy ("Anisotropy", Range(-1,1), Category("Anisotropy")) = 0.5
        Float       AnisotropyRotation ("Anisotropy Rotation", Range(-3.14159,3.14159), Category("Anisotropy")) = 0
        Texture2D   AlbedoTexture ("Albedo Map", Category("Textures"), Intent(Colour))
        // Empty = flat (0.5, 0.5, 1): StandardSurface.shader says why this slot is the one not white.
        Texture2D   NormalTexture ("Normal Map", Category("Textures"), Intent(NormalMap)) = "normal"
        // Packed glTF-style: R = occlusion, G = roughness, B = metallic, each multiplying its factor; white when
        // empty.
        Texture2D   OrmTexture ("ORM Map", Category("Textures"), Intent(PackedMasks))
        Texture2D   EmissiveTexture ("Emissive Map", Category("Textures"), Intent(Colour))
        // Tangent-space stretch direction in RG (2 t - 1, as a normal map's XY); 1x1 = the mesh tangent.
        Texture2D   AnisotropyDirection ("Anisotropy Direction", Category("Anisotropy"), Intent(Data))
    }

    ShadingModel Anisotropic

    // The surface maps: the template declares its own samplers (the cells add no material textures).
    Surface
    {

        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            vec2 tiling = Material.UVTiling;
            if ( tiling.x <= 0.0 ) tiling.x = 1.0;
            if ( tiling.y <= 0.0 ) tiling.y = 1.0;
            SurfaceOutput s = DefaultSurfaceOutput();
            const vec2 uv = SurfaceTransformUV( SurfaceSelectUV( i.UV0, i.UV0, 0 ), Material.UVOffset, tiling,
                                                Material.UVRotation );
            s.BaseColor = SurfaceBaseColor( Material.AlbedoColor.rgb,
                                            SurfaceSampleMaterial( Material.AlbedoTexture, uv ).rgb, vec3( 1.0 ) );
            const ivec2 normalSize = BindlessTextureSize( Material.NormalTexture, 0 );
            if ( normalSize.x > 1 && normalSize.y > 1 )
                s.Normal = SurfaceScaleTangentNormal( SurfaceSampleTangentNormal( Material.NormalTexture, uv ),
                                                      Material.NormalScale );
            const vec3 orm = SurfaceResolveORM( SurfaceSampleMaterial( Material.OrmTexture, uv ).rgb,
                                                Material.AOStrength, Material.RoughnessFactor,
                                                Material.MetallicFactor );
            s.Metallic          = orm.z;
            s.Roughness         = orm.y;
            s.AmbientOcclusion  = orm.x;
            s.Emissive          = SurfaceEmission( SurfaceSampleMaterial( Material.EmissiveTexture, uv ).rgb,
                                                   Material.EmissiveColor.rgb, Material.EmissiveIntensity );
            // The renderer zeroes this row field for objects that must not take the sun's shadow.
            s.ReceiveSunShadows = Material.ReceiveSunShadows;
            // The stretch direction in tangent space: the map's XY where one is bound, else the mesh tangent (+X).
            vec2 direction = vec2( 1.0, 0.0 );
            const ivec2 directionSize = BindlessTextureSize( Material.AnisotropyDirection, 0 );
            if ( directionSize.x > 1 && directionSize.y > 1 )
            {
                const vec2 texel = SurfaceSampleMaterial( Material.AnisotropyDirection, uv ).rg * 2.0 - 1.0;
                if ( dot( texel, texel ) > 1e-6 )
                    direction = normalize( texel );
            }
            const float c = cos( Material.AnisotropyRotation );
            const float r = sin( Material.AnisotropyRotation );
            direction = vec2( c * direction.x - r * direction.y, r * direction.x + c * direction.y );
            const vec3 worldTangent = i.TBN * vec3( direction, 0.0 );
            s.CustomData0       = clamp( Material.Anisotropy, -1.0, 1.0 ) * 0.5 + 0.5;
            s.CustomData1       = DesertEncodeAnisotropyTangent( DesertSurfaceWorldNormal( i, s ), worldTangent );
            s.SampledTextureCount = float( int( BindlessTextureSize( Material.AlbedoTexture, 0 ).x > 1 ) +
                                           int( normalSize.x > 1 ) +
                                           int( BindlessTextureSize( Material.OrmTexture, 0 ).x > 1 ) +
                                           int( BindlessTextureSize( Material.EmissiveTexture, 0 ).x > 1 ) +
                                           int( directionSize.x > 1 ) );
            return s;
        }
    }
}
