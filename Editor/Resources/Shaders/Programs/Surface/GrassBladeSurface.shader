// DesertAsset {"Kind":"Shader","Guid":"2ca20b839c0f4b64a386aa77417e945f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "GrassBladeSurface"
{
    // The procedural grass blade's surface (GRASS-BLADE): the colour is the blade's own - root to tip, the dry tip,
    // the clump's tint, all from the grass variety (Mesh/Surface/Vertex_GrassBlade.glslh writes it as the vertex
    // colour) - tinted by AlbedoColor, and lit by TwoSidedFoliage (UE's MSM_TwoSidedFoliage) so the sun shines
    // through a blade. A blade is a two-sided opaque strip: it has no texture to cut out.

    Domain Surface
    Usage GrassBlades
    TwoSided

    Properties Binding(2)
    {
        Color       AlbedoColor ("Albedo Tint", Category("Surface")) = (1, 1, 1, 1)
        Float       RoughnessFactor ("Roughness", Range(0,1), Category("Surface")) = 0.7
        // UE's Specular pin: a blade's waxy sheen is weaker than a common dielectric's 4 %.
        Float       Specular ("Specular", Range(0,1), Category("Surface")) = 0.3
        // Material half of the sun-shadow receive decision (as StandardSurface).
        Float       ReceiveSunShadows ("Receive Sun Shadows", Range(0,1), Category("Shadows")) = 1
        // The light a blade lets through, as a tint of its own colour (UE's SubsurfaceColor pin).
        Color       SubsurfaceColor ("Subsurface Color", Category("Foliage")) = (0.6, 0.7, 0.35, 1)
    }

    ShadingModel TwoSidedFoliage

    Surface
    {
        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s     = DefaultSurfaceOutput();
            s.BaseColor         = i.VertexColor.rgb * u_Material.AlbedoColor.rgb;
            s.Metallic          = 0.0;
            s.Roughness         = u_Material.RoughnessFactor;
            s.Specular          = u_Material.Specular;
            // The blade darkens towards its root, where its neighbours shade it (UV0.y is 1 at the root).
            s.AmbientOcclusion  = mix( 1.0, 0.45, i.UV0.y );
            s.ReceiveSunShadows = u_Material.ReceiveSunShadows;
            // TwoSidedFoliage's payload: SubsurfaceColor as its square root (UE EncodeSubsurfaceColor).
            const vec3 subsurface = sqrt( clamp( s.BaseColor * u_Material.SubsurfaceColor.rgb, 0.0, 1.0 ) );
            s.CustomData0       = subsurface.r;
            s.CustomData1       = subsurface.g;
            s.CustomData2       = subsurface.b;
            s.CustomData3       = 1.0;
            return s;
        }
    }
}
