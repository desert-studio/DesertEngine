// DesertAsset {"Kind":"Shader","Guid":"87a4759039ec45d192ed65b368ff498e","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:ParticleEmitterSpawn`: the birth of a ParticleEmitterComponent particle (VFX-03i), the
// spawn half of the built-in stack VFX::ParticleEmitterSystem builds from the component's fields. At the emitter,
// moving uniformly inside the cone of half-angle ConeAngle around Direction at StartSpeed less up to SpeedVariance
// of it, living Lifetime less up to LifetimeVariance of it. Positions in centimetres. Spawn group; the update half
// (engine:ParticleEmitterUpdate) gives it its colour and size the same step.
Shader "ParticleEmitterSpawn"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Attribute Velocity vec3
        Attribute LocalOffset vec3
        Attribute Age float
        Attribute Lifetime float

        Input EmitterPosition vec3
        Input Direction vec3
        Input ConeAngle float
        Input StartSpeed float
        Input SpeedVariance float
        Input Lifetime float
        Input LifetimeVariance float

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const vec4 r = VFX_ModuleRandom( sim, i.VFXModuleKey, 0u );

            // Uniform direction inside the cone (radians) around Direction.
            const float cosT  = mix( 1.0, cos( i.ConeAngle ), r.x );
            const float sinT  = sqrt( max( 0.0, 1.0 - cosT * cosT ) );
            const float phi   = 6.28318530718 * r.y;
            const vec3  local = vec3( sinT * cos( phi ), sinT * sin( phi ), cosT );
            const vec3  axis  = normalize( i.Direction + vec3( 1e-5, 0.0, 0.0 ) );
            const vec3  up    = abs( axis.y ) < 0.99 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 );
            const vec3  tang  = normalize( cross( up, axis ) );
            const vec3  bitn  = cross( axis, tang );
            const vec3  dir   = normalize( tang * local.x + bitn * local.y + axis * local.z );

            p.Position    = i.EmitterPosition;
            p.LocalOffset = vec3( 0.0 );
            p.Velocity    = dir * ( i.StartSpeed * ( 1.0 - i.SpeedVariance * r.z ) );
            p.Age         = 0.0;
            p.Lifetime    = max( i.Lifetime * ( 1.0 - i.LifetimeVariance * r.w ), 0.01 );
        }
    }
}
