// DesertAsset {"Kind":"Shader","Guid":"9488cdf80a5f4f4fba16c02d431bc8e0","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:ColorOverLife` (port of UE NiagaraStatelessModule_ScaleColor.ush:5-11): the colour is
// the initial colour times Scale, which is a Curve over the particle's normalised age (VFX-05) or a Value.
// Recomputed from InitialColor every step, so it never compounds. InitializeColor sets the base. Update group.
Shader "VFX/Modules/ColorOverLife"
{
    Domain Particle
    Particle
    {
        Attribute Color vec4
        Attribute InitialColor vec4
        Input Scale vec4

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.Color = p.InitialColor * i.Scale;
        }
    }
}
