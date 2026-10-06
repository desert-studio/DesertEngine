// DesertAsset {"Kind":"Shader","Guid":"d1378c9a2062498ab14e47e7ef29cccb","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:SolveForcesAndVelocity` (UE Niagara Content/Modules/Solvers/SolveForcesAndVelocity):
// the last update module. Integrates the forces the modules above accumulated into the velocity
// (a = F / m, linear drag as a per-second decay), the velocity into the position, ages the particle and
// retires it past its lifetime; the force accumulator is cleared for the next step.
Shader "VFX/Modules/SolveForcesAndVelocity"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Attribute Velocity vec3
        Attribute PhysicsForce vec3
        Attribute PhysicsDrag float
        Attribute Mass float
        Attribute Age float
        Attribute Lifetime float
        Input SpeedLimit float

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const float dt   = sim.DeltaTime;
            const float mass = p.Mass > 0.0 ? p.Mass : 1.0;

            p.Velocity += ( p.PhysicsForce / mass ) * dt;
            p.Velocity *= exp( -max( p.PhysicsDrag, 0.0 ) * dt );

            // SpeedLimit <= 0 means unlimited.
            const float speed = length( p.Velocity );
            if ( i.SpeedLimit > 0.0 && speed > i.SpeedLimit )
                p.Velocity *= i.SpeedLimit / speed;

            p.Position += p.Velocity * dt;
            p.PhysicsForce = vec3( 0.0 );

            p.Age += dt;
            if ( p.Lifetime > 0.0 && p.Age >= p.Lifetime )
                sim.Kill = true;
        }
    }
}
