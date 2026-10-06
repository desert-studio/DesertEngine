// DesertAsset {"Kind":"Shader","Guid":"94456c88cf0e495e9396474e052d53bd","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:UpdateRotation` (port of UE NiagaraStatelessModule_SpriteRotationRate.ush:8-21, stepped):
// the rate first decays by this step's rotational drag (engine:Drag, above this module), then turns the sprite by
// rate times RateScale times the step. RateScale is UE's RateScaleParameters — a Curve over the normalised age
// (VFX-05) or a Value. The angle is kept in [0, 360) so a long life does not lose float precision. Update group.
Shader "VFX/Modules/UpdateRotation"
{
    Domain Particle
    Particle
    {
        Attribute SpriteRotation float
        Attribute SpriteRotationRate float
        Attribute PhysicsRotationalDrag float
        Input RateScale float

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const float dt       = sim.DeltaTime;
            p.SpriteRotationRate *= exp( -max( p.PhysicsRotationalDrag, 0.0 ) * dt );
            p.SpriteRotation      = mod( p.SpriteRotation + p.SpriteRotationRate * i.RateScale * dt, 360.0 );
        }
    }
}
