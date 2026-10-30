// DesertAsset {"Kind":"Shader","Guid":"c4e0017af3dc4187a62e71e3dd4b038d","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SubsurfaceProfileSurface"
{
    // The subsurface-profile surface template (SSS-PROFILE): StandardSurface's maps lit by the SubsurfaceProfile
    // shading model (UE MSM_SubsurfaceProfile) - skin, wax, marble, jade. The material names its profile
    // (MaterialData::SubsurfaceProfile); the renderer writes that profile's atlas row into SubsurfaceProfileIndex
    // (MaterialService), and the template carries it to the G-buffer as the model's CustomData0. No Import rows:
    // no source format states a subsurface profile.
    Domain Surface

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
        Float       ReceiveSunShadows ("Receive Sun Shadows", Range(0,1), Category("Shadows")) = 1
        // UE's Opacity on a subsurface model: the subsurface density a light's contact shadow fades with.
        Float       SubsurfaceOpacity ("Subsurface Opacity", Range(0,1), Category("Subsurface")) = 1
        // WRITTEN BY THE RENDERER, never authored (MaterialService, from MaterialData::SubsurfaceProfile): the
        // profile's row in the atlas, 255 = no profile (UE SSS_PROFILE_ID_INVALID - the texel is not scattered).
        Float       SubsurfaceProfileIndex ("Subsurface Profile Row", Range(0,255), Category("Subsurface")) = 255
        Texture2D   AlbedoTexture ("Albedo Map", Category("Textures"), Intent(Colour))
        Texture2D   NormalTexture ("Normal Map", Category("Textures"), Intent(NormalMap)) = "normal"
        Texture2D   OrmTexture ("ORM Map", Category("Textures"), Intent(PackedMasks))
        Texture2D   EmissiveTexture ("Emissive Map", Category("Textures"), Intent(Colour))
    }

    ShadingModel SubsurfaceProfile

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
                s.Normal = SurfaceScaleTangentNormal( SurfaceSampleTangentNormal( Material.NormalTexture, uv ), Material.NormalScale );
            const vec3 orm = SurfaceResolveORM( SurfaceSampleMaterial( Material.OrmTexture, uv ).rgb, Material.AOStrength,
                                            Material.RoughnessFactor, Material.MetallicFactor );
            s.Metallic          = orm.z;
            s.Roughness         = orm.y;
            s.AmbientOcclusion  = orm.x;
            s.Emissive          = SurfaceEmission( SurfaceSampleMaterial( Material.EmissiveTexture, uv ).rgb,
                                               Material.EmissiveColor.rgb, Material.EmissiveIntensity );
            s.ReceiveSunShadows = Material.ReceiveSunShadows;
            // The SubsurfaceProfile model's payload: the profile row (UE EncodeSubsurfaceProfile) and the opacity.
            s.CustomData0       = clamp( Material.SubsurfaceProfileIndex, 0.0, 255.0 ) / 255.0;
            s.CustomData3       = Material.SubsurfaceOpacity;
            s.SampledTextureCount = float( int( BindlessTextureSize( Material.AlbedoTexture, 0 ).x > 1 ) + int( normalSize.x > 1 ) +
                                           int( BindlessTextureSize( Material.OrmTexture, 0 ).x > 1 ) +
                                           int( BindlessTextureSize( Material.EmissiveTexture, 0 ).x > 1 ) );
            return s;
        }
    }
}
