// DesertAsset {"Kind":"Shader","Guid":"2d5323b9c4d349f8b91c4fc795599f4f","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:InitializeLifetime` (UE InitializeParticle's Lifetime, NiagaraStatelessModule_
// InitializeParticle.ush:20-37 + the emitter's lifetime range): the newborn's age is zero and its lifetime is the
// Lifetime input, seconds — a Random-source range is UE's lifetime min/max. Spawn group; UpdateAge retires it.
Shader "InitializeLifetime"
{
    Domain Particle
    Particle
    {
        Attribute Age float
        Attribute Lifetime float
        Input Lifetime float

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.Age      = 0.0;
            p.Lifetime = max( i.Lifetime, 0.0 );
        }
    }
}
