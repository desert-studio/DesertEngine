// DesertAsset {"Kind":"Shader","Guid":"0f58cf1ce255404d8a2e694ae1f8a429","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:SpawnFromEvent` (VFX-PARAMS; UE's Receive Death / Collision / Location Event modules of
// an emitter Event Handler in Spawned Particles mode). The stack compiler appends it as the LAST spawn module of
// every emitter that states EventHandlers (VFXStackCompiler CompileEmitterStack), so a newborn an event spawned
// takes the event over what the spawn group gave it:
//   Position -> the event's position plus the spawn group's Position (the shape modules place a newborn around
//               zero, so a shape spreads the event's particles around the point the event happened)
//   Velocity -> plus the source particle's velocity times the handler's InheritVelocity (the host scales it)
// A particle not spawned by an event (sim.Event = 0) is untouched.
Shader "SpawnFromEvent"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Attribute Velocity vec3

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            if ( sim.Event == 0u )
                return;
            const VFXEventSpawn e = VFX_EventSpawn( sim.Event - 1u );
            p.Position += e.Position.xyz;
            p.Velocity += e.Velocity.xyz;
        }
    }
}
