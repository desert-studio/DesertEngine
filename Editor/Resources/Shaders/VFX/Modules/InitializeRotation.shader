// DesertAsset {"Kind":"Shader","Guid":"e64f581d72684d28ab4dc2f70e9c807a","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:InitializeRotation` (UE Initialize Particle's Sprite Rotation + the rate of
// NiagaraStatelessModule_SpriteRotationRate.ush:8-21): the newborn's sprite rotation (degrees) and its rotation
// rate (degrees per second). A Random-source range on either is UE's RandomScaleBiasFloat — drawn once, here, so
// it stays the particle's own. Spawn group; UpdateRotation turns it.
Shader "VFX/Modules/InitializeRotation"
{
    Domain Particle
    Particle
    {
        Attribute SpriteRotation float
        Attribute SpriteRotationRate float
        Input Rotation float
        Input RotationRate float

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.SpriteRotation     = mod( i.Rotation, 360.0 );
            p.SpriteRotationRate = i.RotationRate;
        }
    }
}
