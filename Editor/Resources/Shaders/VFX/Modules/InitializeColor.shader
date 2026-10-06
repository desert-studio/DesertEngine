// DesertAsset {"Kind":"Shader","Guid":"dff69e4e958a45f5a4fd4f19ba7de22c","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:InitializeColor` (UE Initialize Particle's Color, NiagaraStatelessModule_
// InitializeParticle.ush): the newborn's linear RGBA colour. A Random-source range is UE's colour min/max. It also
// keeps the colour as InitialColor, the base ColorOverLife scales — a stateful particle cannot re-derive it the way
// a stateless one does. Spawn group.
Shader "VFX/Modules/InitializeColor"
{
    Domain Particle
    Particle
    {
        Attribute Color vec4
        Attribute InitialColor vec4
        Input Color vec4

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.Color        = i.Color;
            p.InitialColor = i.Color;
        }
    }
}
