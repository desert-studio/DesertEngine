// DesertAsset {"Kind":"Shader","Guid":"55b6cdeb3bd78e5c7e30a159ee904baf","Versions":{"SHDR":1},"Dependencies":[]}
// The Toon surface template: StandardSurface's surface (the same maps, the same Properties) lit by the Toon
// shading model (ShadingModels/Toon.shadingmodel) instead of DefaultLit. The model is data — this template names
// it and writes its payload, CustomData0 = Bands and CustomData1 = RimWidth (UE's CustomData pins); no pass and no
// C++ names Toon. It declares no Import block: the importer never picks it, a material chooses it. A cut-out toon
// surface is ToonMasked (BlendMode Masked, UE's Masked blend mode on a Toon material), never this one with a threshold
// parameter: a clip inside EvaluateSurface is a `discard` in every cell, and the ray tracing hit group's closest-hit
// stage has none (glslang refuses it) - the masked template clips in its any-hit stage instead (Pass_RayHit.glslh).
Shader "Toon"
{
    Domain Surface

    // ONE parameter layout for every cell (forward, instanced, GBuffer, skinned): StandardSurface's, plus the
    // two payload floats.
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
        // The Toon model's payload (ShadingModels/Toon.shadingmodel): CustomData0 = bands (0..1 -> 2..8),
        // CustomData1 = rim width.
        Float       Bands ("Bands", Range(0,1), Category("Toon")) = 0.35
        Float       RimWidth ("Rim Width", Range(0,1), Category("Toon")) = 0.4
        Texture2D   AlbedoTexture ("Albedo Map", Category("Textures"))
        // The ONE slot whose empty state is not white. A normal map is unpacked with `2*t - 1`, so a
        // white texel decodes to a normalised (1,1,1) — a normal tilted 54 degrees off the surface —
        // whereas (0.5,0.5,1) decodes to +Z, which is what "this surface has no normal detail" means.
        // The fragment stages here, in StaticMeshGBuffer and in StaticMeshLit_Instanced all guard with
        // `textureSize(NormalTexture,0).x > 1` and skip a 1x1, so this changes no pixel today; it is
        // written down so the guard is a fast path rather than the only thing standing between an empty
        // slot and a wrong normal.
        Texture2D   NormalTexture ("Normal Map", Category("Textures")) = "normal"
        // Packed glTF-style: R = occlusion, G = roughness, B = metallic, each multiplying its factor; white when empty.
        Texture2D   OrmTexture ("ORM Map", Category("Textures"))
        Texture2D   EmissiveTexture ("Emissive Map", Category("Textures"))
    }

    ShadingModel Toon

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
                                        SampleBindless( Material.AlbedoTexture, uv ).rgb, vec3( 1.0 ) );
            const ivec2 normalSize = BindlessTextureSize( Material.NormalTexture, 0 );
            if ( normalSize.x > 1 && normalSize.y > 1 )
                s.Normal = SurfaceScaleTangentNormal( DecodeTangentNormal( SampleBindless( Material.NormalTexture, uv ).rg ), Material.NormalScale );
            const vec3 orm = SurfaceResolveORM( SampleBindless( Material.OrmTexture, uv ).rgb, Material.AOStrength,
                                            Material.RoughnessFactor, Material.MetallicFactor );
            s.Metallic          = orm.z;
            s.Roughness         = orm.y;
            s.AmbientOcclusion  = orm.x;
            s.Emissive          = SurfaceEmission( SampleBindless( Material.EmissiveTexture, uv ).rgb,
                                               Material.EmissiveColor.rgb, Material.EmissiveIntensity );
            // The renderer zeroes this row field for objects that must not take the sun's shadow.
            s.ReceiveSunShadows = Material.ReceiveSunShadows;
            s.CustomData0       = Material.Bands;
            s.CustomData1       = Material.RimWidth;
            s.SampledTextureCount = float( int( BindlessTextureSize( Material.AlbedoTexture, 0 ).x > 1 ) + int( normalSize.x > 1 ) +
                                           int( BindlessTextureSize( Material.OrmTexture, 0 ).x > 1 ) +
                                           int( BindlessTextureSize( Material.EmissiveTexture, 0 ).x > 1 ) );
            return s;
        }
    }
}
