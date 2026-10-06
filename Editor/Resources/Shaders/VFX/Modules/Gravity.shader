// DesertAsset {"Kind":"Shader","Guid":"bd19bb728e4646919475240f1a19369d","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:Gravity` (UE Niagara Content/Modules/Update/Forces/GravityForce): adds a constant
// acceleration to the particle's accumulated force. Centimetres per second squared (world units = cm).
// SolveForcesAndVelocity integrates the sum.
Shader "VFX/Modules/Gravity"
{
    Domain Particle
    Particle
    {
        Attribute PhysicsForce vec3
        Attribute Mass float
        Input Gravity vec3

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            // A force, so it is acceleration times mass; a particle never given a mass weighs 1.
            p.PhysicsForce += i.Gravity * ( p.Mass > 0.0 ? p.Mass : 1.0 );
        }
    }
}
