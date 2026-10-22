// DesertAsset {"Kind":"Shader","Guid":"d3d0dbcaed414e45aff3a35d204e24ae","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleMeshInstances"
{
    Compute
    {
        // "Particles: Mesh Instances {emitter}.{row}" (VFX-09): one Mesh renderer row's GPU instances
        // (GpuInstances.hpp, UE NiagaraMeshVertexFactory reading the particle's position / scale / orientation).
        // Thread t < alive writes the transform of the t-th particle the last compact found alive:
        // T(world position) * R(MeshOrientation) * S(Scale attribute * the row's Scale), the InstanceTransforms of
        // Mesh/Surface/Vertex_Instanced.glslh. Thread 0 writes every submesh record's instanceCount (= alive) and
        // firstInstance (0), so the mesh renderer's indexed-indirect draws read the count the GPU holds - no
        // readback. The alive count is the draw slot's (six vertices per particle, ParticleCompact).
        LocalSize(64, 1, 1);

        #include <Common/ParticlePool.glslh>
        #include <Common/ParticleDrawLayout.glslh>

        ReadBuffer(0) AttributeFloats
        {
            float u_DrawFloats[];
        };

        ReadBuffer(5) DrawLayout
        {
            ParticleDrawLayout u_DrawLayout;
        };

        #include <Common/ParticleDrawRead.glslh>

        ReadBuffer(1) AliveList
        {
            uint u_Alive[];
        };

        ReadBuffer(2) Counters
        {
            ParticleDrawSlot u_Slots[];
        };

        Buffer(3) Instances
        {
            mat4 u_Instances[];
        };

        Buffer(4) Args
        {
            uint u_Args[]; // GpuInstances.hpp: header, then 5 words per submesh record
        };

        PushConstant PushConstants
        {
            uvec4 u_Range; // x = the alive half's offset in AliveList, y = the draw slot, z = submesh count, w = capacity
            vec4  u_Scale; // xyz = the row's Scale
        };

        void main()
        {
            uint alive = min( u_Slots[u_Range.y].VertexCount / 6u, u_Range.w );
            uint t     = gl_GlobalInvocationID.x;
            if ( t == 0u )
            {
                for ( uint s = 0u; s < u_Range.z; ++s )
                {
                    u_Args[4u + s * 5u + 1u] = alive;
                    u_Args[4u + s * 5u + 4u] = 0u;
                }
                u_Args[3] = alive;
            }
            if ( t >= alive )
                return;
            uint n     = ParticleLocalIndex( u_Alive[u_Range.x + t] );
            vec4 q     = ParticleMeshOrientation( n );
            float len  = length( q );
            q          = len > 0.0 ? q / len : vec4( 0.0, 0.0, 0.0, 1.0 );
            vec3 scale = ParticleMeshScale( n ) * u_Scale.xyz;
            // The quaternion's rotation matrix, column by column (x, y, z axes).
            vec3 x = vec3( 1.0 - 2.0 * ( q.y * q.y + q.z * q.z ), 2.0 * ( q.x * q.y + q.w * q.z ), 2.0 * ( q.x * q.z - q.w * q.y ) );
            vec3 y = vec3( 2.0 * ( q.x * q.y - q.w * q.z ), 1.0 - 2.0 * ( q.x * q.x + q.z * q.z ), 2.0 * ( q.y * q.z + q.w * q.x ) );
            vec3 z = vec3( 2.0 * ( q.x * q.z + q.w * q.y ), 2.0 * ( q.y * q.z - q.w * q.x ), 1.0 - 2.0 * ( q.x * q.x + q.y * q.y ) );
            u_Instances[t] = mat4( vec4( x * scale.x, 0.0 ), vec4( y * scale.y, 0.0 ), vec4( z * scale.z, 0.0 ),
                                   vec4( ParticleWorldPosition( n ), 1.0 ) );
        }
    }
}
