// DesertAsset {"Kind":"Shader","Guid":"8079c58d9d214db694607abaf9c1a4b2","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:ShapePoint` (UE Shape Location, primitive Point): the particle is born at the emitter's
// origin plus Offset (centimetres). Spawn group.
Shader "VFX/Modules/ShapePoint"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Input Offset vec3

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.Position += i.Offset;
        }
    }
}
