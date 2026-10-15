// DesertAsset {"Kind":"Shader","Guid":"27403cd838f8422baa658df0e1b115e0","Versions":{"SHDR":1},"Dependencies":[]}
// The engine's ribbon template (UE's DefaultRibbonMaterial): what a Ribbon renderer row draws with when it names
// no material, and what ParticleRenderer falls back to BY NAME (logged) when the named one has no
// ParticleRibbon.Forward cell. Translucent: the particle's colour, fading out across the strip's edges.
Shader "ParticleRibbonDefault"
{
    Domain Surface
    Usage ParticleRibbons
    BlendMode Translucent
    // UE bResponsiveAA: a ribbon writes no velocity of its own.
    ResponsiveAA

    Properties Binding(2)
    {
        Color Tint ("Tint") = (1, 1, 1, 1)
    }

    State
    {
        Cull None
        ZTest LEqual
        ZWrite Off
    }

    ShadingModel Unlit

    Surface
    {
        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s = DefaultSurfaceOutput();
            const float   edge = 1.0 - abs( i.UV0.y * 2.0 - 1.0 );
            s.BaseColor        = vec3( 0.0 );
            s.Emissive         = i.Particle.Color.rgb * u_Material.Tint.rgb;
            s.Opacity          = i.Particle.Color.a * u_Material.Tint.a * smoothstep( 0.0, 0.5, edge );
            return s;
        }
    }
}
