// DesertAsset {"Kind":"Shader","Guid":"3435fcff54c84b3499fa6ed992a4bfd3","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:ShapeSphere` (port of UE NiagaraStatelessModule_ShapeLocation.ush:97-108, Sphere): a
// uniformly random direction (RandomUnitFloat3) times Radius. Radius is an ordinary input, so UE's
// RandomScaleBiasFloat(SphereScale, SphereBias) is a Random-source range on it (Min = inner, Max = outer radius),
// a Value is a shell. Centimetres. Spawn group.
Shader "VFX/Modules/ShapeSphere"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Input Radius float
        Input Offset vec3

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const vec3 direction = VFX_RandomUnitVector( VFX_ModuleRandom( sim, i.VFXModuleKey, 0u ) );
            p.Position += i.Offset + direction * i.Radius;
        }
    }
}
