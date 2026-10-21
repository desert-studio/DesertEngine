// DesertAsset {"Kind":"Shader","Guid":"1edce1e9d4454b87b612f34b1f6b22a8","Versions":{"SHDR":1},"Dependencies":[]}
// The engine's translucent particle RIBBON template (UE's DefaultRibbonMaterial): what a Ribbon renderer row draws
// with when it names no material, and what ParticleRenderer falls back to BY NAME (with a logged error naming the
// emitter and the material) when the material it names has no ribbon cell. `Usage ParticleRibbons` gives it the one
// ParticleRibbon.Forward cell (MeshVertexPath::ParticleRibbon); BlendMode Translucent = laid OVER the scene.
// The look: the particle's simulated colour, soft towards both edges of the strip (V = 0 / 1 across its width).
Shader "ParticleRibbonDefault"
{
    Domain Surface
    Usage ParticleRibbons
    BlendMode Translucent
    // UE bResponsiveAA: a ribbon writes no velocity of its own; its coverage tells TAA to trust the current frame.
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
            // 1 along the strip's centre line, 0 at its two edges, smooth in between.
            const float across  = abs( i.UV0.y * 2.0 - 1.0 );
            const float edge    = 1.0 - smoothstep( 0.0, 1.0, across );
            const vec4  colour  = i.Particle.Color * u_Material.Tint;
            SurfaceOutput s     = DefaultSurfaceOutput();
            s.BaseColor         = vec3( 0.0 );
            s.Emissive          = colour.rgb;
            s.Opacity           = colour.a * edge;
            return s;
        }
    }
}
