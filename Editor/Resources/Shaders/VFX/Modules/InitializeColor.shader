// DesertAsset {"Kind":"Shader","Guid":"dff69e4e958a45f5a4fd4f19ba7de22c","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:InitializeColor` (UE Initialize Particle's Color, NiagaraStatelessModule_
// InitializeParticle.ush): the newborn's linear RGBA colour. A Random-source range is UE's colour min/max. It also
// keeps the colour as InitialColor, the base ColorOverLife scales — a stateful particle cannot re-derive it the way
// a stateless one does. A Spawn from Channel colour, when the emitter binds one, replaces the input (UE: the
// colour read from the Niagara Data Channel). Spawn group.
Shader "InitializeColor"
{
    Domain Particle
    Particle
    {
        Attribute Color vec4
        Attribute InitialColor vec4
        Input Color vec4 Range=0..1 Default=1,1,1,1

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const vec4 colour = sim.ChannelScalars.z > 0.5 ? sim.ChannelColor : i.Color;
            p.Color        = colour;
            p.InitialColor = colour;
        }
    }
}
