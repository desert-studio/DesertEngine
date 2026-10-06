// DesertAsset {"Kind":"Shader","Guid":"240adf0c6a0f407489419764780491eb","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:SizeOverLife` (port of UE NiagaraStatelessModule_ScaleSpriteSize.ush:6-16): the sprite
// size is the initial size times Scale, a Curve over the normalised age (VFX-05) or a Value — UE's CurveScale is
// folded into the curve's keys. Recomputed from InitialSpriteSize every step, so it never compounds. Update group.
Shader "VFX/Modules/SizeOverLife"
{
    Domain Particle
    Particle
    {
        Attribute SpriteSize vec2
        Attribute InitialSpriteSize vec2
        Input Scale vec2

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.SpriteSize = max( p.InitialSpriteSize * i.Scale, vec2( 0.0 ) );
        }
    }
}
