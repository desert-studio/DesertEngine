// DesertAsset {"Kind":"Shader","Guid":"0caef28c515f4a4e93363f1616ed3ca3","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:UpdateAge` (UE Content Modules/Update/Lifetime "Particle State"): ages the particle by
// the step and retires it once its age reaches its lifetime. A particle with no lifetime (InitializeLifetime not in
// the stack) never dies of age. Update group.
Shader "VFX/Modules/UpdateAge"
{
    Domain Particle
    Particle
    {
        Attribute Age float
        Attribute Lifetime float

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.Age += sim.DeltaTime;
            if ( p.Lifetime > 0.0 && p.Age >= p.Lifetime )
                sim.Kill = true;
        }
    }
}
