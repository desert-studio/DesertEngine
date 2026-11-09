// DesertAsset {"Kind":"Shader","Guid":"d649a1cb12ffed49f8bcf432e3ae32af","Versions":{"SHDR":1},"Dependencies":[]}
// The clear-coat surface template (UE: a material with Shading Model Clear Coat): StandardSurface's maps lit by
// the ClearCoat shading model (ShadingModels/ClearCoat.shadingmodel) - a clear lacquer over the base, for car
// paint, varnish and coated plastics. The template names the model and writes its payload, CustomData0 = Clear
// Coat and CustomData1 = Clear Coat Roughness (UE's two pins); the coat's strength may come from a mask map (R) so
// a decal strip or worn edge loses its lacquer. It declares no Import block: the importer never picks it, a
// material does.
Shader "ClearCoatSurface"
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
        // The ClearCoat model's payload (ShadingModels/ClearCoat.shadingmodel): the coat's strength and roughness.
        Float       ClearCoat ("Clear Coat", Range(0,1), Category("Clear Coat")) = 1
        Float       ClearCoatRoughness ("Clear Coat Roughness", Range(0,1), Category("Clear Coat")) = 0.1
        Texture2D   AlbedoTexture ("Albedo Map", Category("Textures"), Intent(Colour))
        // Empty = flat (0.5, 0.5, 1): StandardSurface.shader says why this slot is the one not white.
        Texture2D   NormalTexture ("Normal Map", Category("Textures"), Intent(NormalMap)) = "normal"
        // Packed glTF-style: R = occlusion, G = roughness, B = metallic, each multiplying its factor; white when
        // empty.
        Texture2D   OrmTexture ("ORM Map", Category("Textures"), Intent(PackedMasks))
        Texture2D   EmissiveTexture ("Emissive Map", Category("Textures"), Intent(Colour))
        // R multiplies Clear Coat (white when empty: the factor alone).
        Texture2D   ClearCoatMask ("Clear Coat Mask", Category("Clear Coat"), Intent(Data))
    }

    ShadingModel ClearCoat

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
            s.CustomData0       = Material.ClearCoat * SurfaceSampleMaterial( Material.ClearCoatMask, uv ).r;
            s.CustomData1       = Material.ClearCoatRoughness;
            s.SampledTextureCount = float( int( BindlessTextureSize( Material.AlbedoTexture, 0 ).x > 1 ) +
                                           int( normalSize.x > 1 ) +
                                           int( BindlessTextureSize( Material.OrmTexture, 0 ).x > 1 ) +
                                           int( BindlessTextureSize( Material.EmissiveTexture, 0 ).x > 1 ) +
                                           int( BindlessTextureSize( Material.ClearCoatMask, 0 ).x > 1 ) );
            return s;
        }
    }
}
