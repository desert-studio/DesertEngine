// DesertAsset {"Kind":"Shader","Guid":"efeb2a01d95840218261adfddef6174f","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:SpawnFromChannel` (VFX-10, port of UE Niagara "Spawn From Data Channel"), its particle
// half. The emitter half is CPU administration (EmitterUpdate group, VFX/VFXEmitterSpawn.cpp ReadChannelSpawn): it
// turns this frame's entries of the named channel into spawns (VFX::GatherChannelSpawns, one row per particle,
// VFX::ExpandChannelSpawns). The stack compiler appends THIS file to the end of the spawn group of every emitter
// whose EmitterUpdate holds the module, so a channel particle takes its entry's bound fields over whatever the
// spawn group gave it:
//   Position  -> Position (world centimetres)
//   Direction -> re-aims Velocity: the spawn group's velocity, read as built around +Z (AddVelocityInCone Axis
//                0,0,1), turned so +Z is the entry's direction (UE: a cone's Axis bound to the channel)
//   Color     -> multiplies Color
//   Lifetime  -> Lifetime (seconds)
//   Size      -> scales SpriteSize
// A field the module does not bind leaves its attribute alone; a particle not spawned from the channel is untouched.
Shader "SpawnFromChannel"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Attribute Velocity vec3
        Attribute Color vec4
        Attribute Lifetime float
        Attribute SpriteSize vec2

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            if ( sim.Channel == 0u )
                return;
            const VFXChannelSpawn c = VFX_ChannelSpawn( sim.Channel - 1u );
            if ( c.Position.w > 0.5 )
                p.Position = c.Position.xyz;
            if ( c.Direction.w > 0.5 && dot( c.Direction.xyz, c.Direction.xyz ) > 1e-12 )
                p.Velocity = VFX_RotateZTo( normalize( c.Direction.xyz ), p.Velocity );
            if ( c.Scalars.z > 0.5 )
                p.Color *= c.Color;
            if ( c.Scalars.w > 0.5 )
                p.Lifetime = max( c.Scalars.x, 0.01 );
            p.SpriteSize *= c.Scalars.y;
        }
    }
}
