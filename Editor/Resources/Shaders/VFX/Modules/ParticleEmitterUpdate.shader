// DesertAsset {"Kind":"Shader","Guid":"98b3e889011e4d54af913049f7104c00","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:ParticleEmitterUpdate`: the life of a ParticleEmitterComponent particle (VFX-03i), the
// update half of the built-in stack VFX::ParticleEmitterSystem builds from the component's fields. A particle older
// than its birth step falls under Gravity and moves by its velocity - in LocalSpace as an offset that rides with the
// emitter at EmitterPosition - ages, and dies when its age reaches its lifetime. Every particle, the newborn too,
// then takes its colour (StartColor -> EndColor) and size (StartSize -> EndSize, eased by SizeCurvePower) at its
// normalised age, which it keeps as NormalizedAge for the sprite renderer. Centimetres. Update group.
Shader "ParticleEmitterUpdate"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Attribute Velocity vec3
        Attribute LocalOffset vec3
        Attribute Age float
        Attribute Lifetime float
        Attribute NormalizedAge float
        Attribute Color vec4
        Attribute SpriteSize vec2

        Input EmitterPosition vec3
        Input Gravity vec3
        Input LocalSpace bool
        Input StartColor vec4
        Input EndColor vec4
        Input StartSize float
        Input EndSize float
        Input SizeCurvePower float

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            if ( !sim.Spawned )
            {
                const float dt = sim.DeltaTime;
                p.Velocity += i.Gravity * dt;
                if ( i.LocalSpace )
                {
                    p.LocalOffset += p.Velocity * dt;
                    p.Position = i.EmitterPosition + p.LocalOffset;
                }
                else
                {
                    p.Position += p.Velocity * dt;
                }
                p.Age += dt;
                if ( p.Age >= p.Lifetime )
                    sim.Kill = true;
            }

            const float t   = p.Lifetime > 0.0 ? clamp( p.Age / p.Lifetime, 0.0, 1.0 ) : 0.0;
            const float st  = pow( t, i.SizeCurvePower > 0.0 ? i.SizeCurvePower : 1.0 );
            p.NormalizedAge = t;
            p.SpriteSize    = vec2( mix( i.StartSize, i.EndSize, st ) );
            p.Color         = mix( i.StartColor, i.EndColor, t );
        }
    }
}
