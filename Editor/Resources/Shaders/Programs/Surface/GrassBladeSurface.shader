// DesertAsset {"Kind":"Shader","Guid":"2ca20b839c0f4b64a386aa77417e945f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "GrassBladeSurface"
{
    // The grass blade's surface (GRASS-ASSET): a card cut from a scanned blade atlas - UE's Landscape Grass Type
    // draws a foliage material on its grass, and so does this. Albedo, normal, ORM and opacity are the scan's
    // (Poly Haven CC0: veins, ragged edge, dry tips are in the texture); the strip the vertex stage bends
    // (Mesh/Surface/Vertex_GrassBlade.glslh) brings UV0 = the card's place on the atlas and UV1 = (the clump's
    // draw, the height share). Lit by TwoSidedFoliage (UE's MSM_TwoSidedFoliage) so the sun shines through, cut
    // out by the opacity map (UE foliage is BlendMode Masked).

    Domain Surface
    Usage GrassBlades
    TwoSided

    Properties Binding(2)
    {
        Color       AlbedoColor ("Albedo Tint", Category("Surface")) = (1, 1, 1, 1)
        Float       RoughnessFactor ("Roughness", Range(0,1), Category("Surface")) = 1
        Float       Specular ("Specular", Range(0,1), Category("Surface")) = 0.5
        Float       AOStrength ("Ambient Occlusion", Range(0,1), Category("Surface")) = 1
        Float       NormalScale ("Normal Scale", Range(0,4), Category("Surface")) = 1
        // UE's Opacity Mask Clip Value: a texel whose mask is below it is cut in every pass.
        Float       OpacityMaskClipValue ("Opacity Mask Clip Value", Range(0,1), Category("Surface")) = 0.35
        // Which channel of OpacityTexture is the mask (0 = R of a separate opacity map, 3 = A of the albedo).
        Float       OpacityChannel ("Opacity Channel", Range(0,3), Category("Surface")) = 0
        // The clump's tint: each clump draws one point between these two, so a meadow reads as patches of one
        // grass and not one green (GoT's clump colour, here the material's to author).
        Color       ClumpTintCool ("Clump Tint Cool", Category("Field")) = (1, 1, 1, 1)
        Color       ClumpTintWarm ("Clump Tint Warm", Category("Field")) = (1, 1, 1, 1)
        // The occlusion at a blade's root, where its neighbours shade it, rising to 1 at the tip (UE grass
        // materials darken toward the root by the vertex's height the same way).
        Float       RootOcclusion ("Root Occlusion", Range(0,1), Category("Field")) = 1
        // The scan's mean colour over the cards the blades draw (AlbedoTexture weighted by OpacityTexture, linear):
        // what the landscape's far grass cover (Common/LandscapeGrassCover.glslh) carries on once the blades are
        // thinned out. A value derived from the textures, not authored: measured from the scan for now, the cook
        // is to derive it (GRASS-ASSET REMAINDER).
        Color       CoverAlbedo ("Cover Albedo", Category("Field")) = (0, 0, 0, 1)
        // Material half of the sun-shadow receive decision (as StandardSurface).
        Float       ReceiveSunShadows ("Receive Sun Shadows", Range(0,1), Category("Shadows")) = 1
        // The light a blade lets through, as a tint of its own albedo (UE's SubsurfaceColor pin).
        Color       SubsurfaceColor ("Subsurface Color", Category("Foliage")) = (1, 1, 1, 1)
        Texture2D   AlbedoTexture ("Albedo Map", Category("Textures"), Intent(Colour))
        Texture2D   NormalTexture ("Normal Map", Category("Textures"), Intent(NormalMap)) = "normal"
        Texture2D   OpacityTexture ("Opacity Map", Category("Textures"), Intent(Mask))
        // Packed glTF-style: R = occlusion, G = roughness, B = metallic.
        Texture2D   OrmTexture ("ORM Map", Category("Textures"), Intent(PackedMasks))
    }

    ShadingModel TwoSidedFoliage
    BlendMode Masked
    OpacityMask OpacityTexture Channel(OpacityChannel) UV(0)

    Surface
    {
        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s   = DefaultSurfaceOutput();
            const vec2    uv  = i.UV0;
            const vec3    tint = mix( Material.ClumpTintCool.rgb, Material.ClumpTintWarm.rgb, i.UV1.x );
            s.BaseColor = SurfaceBaseColor( Material.AlbedoColor.rgb * tint,
                                            SurfaceSampleMaterial( Material.AlbedoTexture, uv ).rgb, vec3( 1.0 ) );
            const ivec2 normalSize = BindlessTextureSize( Material.NormalTexture, 0 );
            if ( normalSize.x > 1 && normalSize.y > 1 )
                s.Normal = SurfaceScaleTangentNormal( SurfaceSampleTangentNormal( Material.NormalTexture, uv ),
                                                      Material.NormalScale );
            const vec3 orm = SurfaceResolveORM( SurfaceSampleMaterial( Material.OrmTexture, uv ).rgb,
                                                Material.AOStrength, Material.RoughnessFactor, 0.0 );
            s.Metallic          = 0.0;
            s.Roughness         = orm.y;
            s.Specular          = Material.Specular;
            s.AmbientOcclusion  = orm.x * mix( Material.RootOcclusion, 1.0, i.UV1.y );
            s.ReceiveSunShadows = Material.ReceiveSunShadows;
            // TwoSidedFoliage's payload: SubsurfaceColor as its square root (UE EncodeSubsurfaceColor).
            const vec3 subsurface = sqrt( clamp( s.BaseColor * Material.SubsurfaceColor.rgb, 0.0, 1.0 ) );
            s.CustomData0       = subsurface.r;
            s.CustomData1       = subsurface.g;
            s.CustomData2       = subsurface.b;
            s.CustomData3       = 1.0;
            s.SampledTextureCount = float( int( BindlessTextureSize( Material.AlbedoTexture, 0 ).x > 1 ) +
                                           int( normalSize.x > 1 ) +
                                           int( BindlessTextureSize( Material.OpacityTexture, 0 ).x > 1 ) +
                                           int( BindlessTextureSize( Material.OrmTexture, 0 ).x > 1 ) );
            return s;
        }
    }
}
