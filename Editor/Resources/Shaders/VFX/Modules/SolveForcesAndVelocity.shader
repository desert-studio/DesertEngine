// DesertAsset {"Kind":"Shader","Guid":"d1378c9a2062498ab14e47e7ef29cccb","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:SolveForcesAndVelocity` (UE Niagara Content/Modules/Solvers/SolveForcesAndVelocity):
// the last update module. Integrates the force the modules above accumulated (a = F / m) under this step's linear
// drag exactly over the step — port of UE NiagaraStatelessModule_SolveVelocitiesAndForces.ush:76-90
// (IntegratePosition): with drag k the velocity relaxes towards the terminal velocity a / k,
// v' = a/k + (v - a/k) e^(-k dt), x' = x + (a/k) dt + (v - a/k)(1 - e^(-k dt)) / k; without drag it is Newtonian,
// x' = x + v dt + a dt^2 / 2. SpeedLimit (<= 0: unlimited) caps the new speed, and a capped step moves at the capped
// velocity. The step's accumulators — force, drag, rotational drag — are cleared for the next step. Ageing and
// retiring the particle is UpdateAge's (UE: Particle State), not the solver's.
Shader "SolveForcesAndVelocity"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Attribute Velocity vec3
        Attribute PhysicsForce vec3
        Attribute PhysicsDrag float
        Attribute PhysicsRotationalDrag float
        Attribute Mass float
        Input SpeedLimit float

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const float dt    = sim.DeltaTime;
            const float mass  = p.Mass > 0.0 ? p.Mass : 1.0;
            const vec3  accel = p.PhysicsForce / mass;
            const float drag  = max( p.PhysicsDrag, 0.0 );

            vec3 velocity;
            vec3 travel;
            if ( drag > 0.0001 )
            {
                const vec3  terminal = accel / drag;
                const float decay    = exp( -drag * dt );
                velocity             = terminal + ( p.Velocity - terminal ) * decay;
                travel               = terminal * dt + ( p.Velocity - terminal ) * ( ( 1.0 - decay ) / drag );
            }
            else
            {
                velocity = p.Velocity + accel * dt;
                travel   = p.Velocity * dt + 0.5 * accel * dt * dt;
            }

            const float speed = length( velocity );
            if ( i.SpeedLimit > 0.0 && speed > i.SpeedLimit )
            {
                velocity *= i.SpeedLimit / speed;
                travel = velocity * dt;
            }

            p.Position += travel;
            p.Velocity              = velocity;
            p.PhysicsForce          = vec3( 0.0 );
            p.PhysicsDrag           = 0.0;
            p.PhysicsRotationalDrag = 0.0;
        }
    }
}
