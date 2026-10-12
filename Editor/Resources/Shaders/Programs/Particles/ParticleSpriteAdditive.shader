// DesertAsset {"Kind":"Shader","Guid":"a3e81f6c07d24b5c9e1f40b8d26c7a13","Versions":{"SHDR":1},"Dependencies":[]}
// The engine's additive particle sprite template (UE's DefaultSpriteMaterial): what an emitter draws with when it
// names no material, and what ParticleRenderer falls back to BY NAME (with a logged error naming the emitter and
// the material) when the material it names cannot draw sprites. `Usage ParticleSprites` gives it the one
// ParticleSprite.Forward cell (MeshVertexPath::ParticleSprite); BlendMode Additive = ADDED to the scene (SrcAlpha, One): fire, sparks, glows.
// The look is the billboard this replaced: a radial soft dot in the particle's simulated colour.
Shader "ParticleSpriteAdditive"
{
    Domain Surface
    Usage ParticleSprites
    BlendMode Additive

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
            // 1 at the centre of the quad, 0 at its inscribed circle, smooth in between.
            const float r       = length( i.UV0 * 2.0 - 1.0 );
            const float disc    = 1.0 - smoothstep( 0.0, 1.0, r );
            const vec4  colour  = i.Particle.Color * u_Material.Tint;
            SurfaceOutput s     = DefaultSurfaceOutput();
            s.BaseColor         = vec3( 0.0 );
            s.Emissive          = colour.rgb;
            s.Opacity           = colour.a * disc;
            return s;
        }
    }
}
