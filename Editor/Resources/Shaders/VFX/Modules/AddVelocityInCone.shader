// DesertAsset {"Kind":"Shader","Guid":"37ef32a37dfc4501bb57e95fc9919372","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:AddVelocityInCone` (port of UE NiagaraStatelessModule_SolveVelocitiesAndForces.ush:
// 104-121, cone velocity): a direction at an angle uniform in [0, Angle] (degrees) from Axis, at a uniform
// rotation around it, times Speed (cm/s); Falloff in [0, 1] slows the directions far from the axis
// (UE: Speed * lerp(1, cos(angle)^(Falloff * 10), Falloff)). Speed and Angle are ordinary inputs — a Random-source
// range is UE's RandomScaleBiasFloat on them. Adds to the velocity, so it stacks. Spawn group.
Shader "VFX/Modules/AddVelocityInCone"
{
    Domain Particle
    Particle
    {
        Attribute Velocity vec3
        Input Speed float
        Input Angle float
        Input Falloff float
        Input Axis vec3

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const vec4  r         = VFX_ModuleRandom( sim, i.VFXModuleKey, 0u );
            const float angle     = radians( i.Angle ) * r.x;
            const float rotation  = 6.28318530718 * r.y;
            const vec2  scAngle   = vec2( sin( angle ), cos( angle ) );
            const vec3  direction = vec3( sin( rotation ) * scAngle.x, cos( rotation ) * scAngle.x, scAngle.y );

            float speed = i.Speed;
            if ( i.Falloff > 0.0 )
                speed *= mix( 1.0, pow( clamp( scAngle.y, 0.0, 1.0 ), i.Falloff * 10.0 ), i.Falloff );
            p.Velocity += VFX_RotateZTo( i.Axis, direction ) * speed;
        }
    }
}
