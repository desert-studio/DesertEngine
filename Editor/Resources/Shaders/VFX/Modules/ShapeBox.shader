// DesertAsset {"Kind":"Shader","Guid":"022360554da04ff3b498ee101948b24a","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:ShapeBox` (port of UE NiagaraStatelessModule_ShapeLocation.ush:12-40, Box): a uniform
// point inside a box of Size centred on the origin, or — SurfaceOnly — on one of its six faces picked uniformly,
// the face pushed out by up to Thickness (UE: BoxSize + Thickness * P0.z). UE's box rotation is the simulation
// space's business here (Local space rides the emitter). Centimetres. Spawn group.
Shader "VFX/Modules/ShapeBox"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Input Size vec3
        Input SurfaceOnly bool
        Input Thickness vec3
        Input Offset vec3

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const vec3 p0       = VFX_ModuleRandom( sim, i.VFXModuleKey, 0u ).xyz - 0.5;
            vec3       location = p0 * i.Size;
            if ( i.SurfaceOnly )
            {
                const vec3 faceA[6] = vec3[]( vec3( 0, 1, 0 ), vec3( 0, 1, 0 ), vec3( 0, 0, 1 ), vec3( 0, 0, 1 ),
                                              vec3( 1, 0, 0 ), vec3( 1, 0, 0 ) );
                const vec3 faceB[6] = vec3[]( vec3( 0, 0, 1 ), vec3( 0, 0, 1 ), vec3( 1, 0, 0 ), vec3( 1, 0, 0 ),
                                              vec3( 0, 1, 0 ), vec3( 0, 1, 0 ) );
                const vec3 faceC[6] = vec3[]( vec3( 1, 0, 0 ), vec3( -1, 0, 0 ), vec3( 0, 1, 0 ), vec3( 0, -1, 0 ),
                                              vec3( 0, 0, 1 ), vec3( 0, 0, -1 ) );
                const vec3 size = i.Size + i.Thickness * p0.z;
                const uint face = min( uint( VFX_ModuleRandom( sim, i.VFXModuleKey, 1u ).x * 6.0 ), 5u );
                location = size * faceA[face] * p0.x + size * faceB[face] * p0.y + size * faceC[face] * 0.5;
            }
            p.Position += i.Offset + location;
        }
    }
}
