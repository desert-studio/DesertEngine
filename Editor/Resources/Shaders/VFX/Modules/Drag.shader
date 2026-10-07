// DesertAsset {"Kind":"Shader","Guid":"a0f042db0b6248cab1bcf38605abbf78","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:Drag` (UE Niagara Content/Modules/Update/Forces/Drag): adds a linear drag and a
// rotational drag, both per second, to the particle's accumulators for this step. SolveForcesAndVelocity turns the
// linear one into the exact exponential solve (port of NiagaraStatelessModule_SolveVelocitiesAndForces.ush:76-90:
// terminal velocity a / drag, lambda = (1 - e^(-drag dt)) / drag); UpdateRotation decays the rotation rate by the
// rotational one. Mass-independent, as in UE. The solver clears both after the step. Update group.
Shader "Drag"
{
    Domain Particle
    Particle
    {
        Attribute PhysicsDrag float
        Attribute PhysicsRotationalDrag float
        Input Drag float
        Input RotationalDrag float

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.PhysicsDrag += max( i.Drag, 0.0 );
            p.PhysicsRotationalDrag += max( i.RotationalDrag, 0.0 );
        }
    }
}
