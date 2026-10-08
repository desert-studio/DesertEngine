// DesertAsset {"Kind":"Shader","Guid":"faf8cbc20f5b43a4b3d6ecd7d35047d5","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:ShapeCone` (UE Shape Location, primitive Cone): a point at distance Length from the
// apex along a direction inside the cone of half-angle Angle (degrees) around Axis — the direction drawn as UE's
// cone velocity draws it (NiagaraStatelessModule_SolveVelocitiesAndForces.ush:104-121: angle uniform in
// [0, Angle], rotation uniform around the axis). Length is an ordinary input: a Random-source range fills the
// cone's volume between two distances, a Value is its cap. Centimetres. Spawn group.
Shader "ShapeCone"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Input Angle float
        Input Length float
        Input Axis vec3
        Input Offset vec3

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const vec4  r         = VFX_ModuleRandom( sim, i.VFXModuleKey, 0u );
            const float angle     = radians( i.Angle ) * r.x;
            const float rotation  = 6.28318530718 * r.y;
            const vec3  direction = vec3( sin( rotation ) * sin( angle ), cos( rotation ) * sin( angle ), cos( angle ) );
            p.Position += i.Offset + VFX_RotateZTo( i.Axis, direction ) * i.Length;
        }
    }
}
