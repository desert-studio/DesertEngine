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
        // Motion (UE NiagaraMeshVertexFactory's PrevPosition / PrevScale / PrevRotation): u_Instances[capacity + t]
        // is the t-th instance's transform at the previous view frame, from the particle's History row (indexed by
        // its range-local slot, which the alive list keeps across frames). The row counts only if it was written at
        // the previous frame (u_Frame.x - 1) and by the same particle: the row keeps the pool slot's Id, which
        // every spawn writes anew (ParticleSimulate IdBase + t, UE Niagara's persistent particle ID), so a slot
        // freed and respawned between two frames never reads the dead particle's row - with or without an Age
        // attribute. Otherwise (just spawned, or not drawn last frame) the previous transform is this frame's: no
        // motion. Mesh/Surface/Vertex_Instanced.glslh reads it at the push's previous-instance
        // offset (= capacity).
        // LOD (UE Niagara Mesh Renderer, a level by screen size; Geometry::SelectLODFromBounds is the same rule
        // for a mesh component): u_Lod.z = 0 - each live particle measures its world bounding sphere (the mesh's
        // local sphere through its transform) on screen and atomicMin's the coarsest level it is still below into
        // the reduction word; u_Lod.z = 1 (one thread, the next pass) - copies that level's index ranges from
        // the level table into every submesh record and re-arms the word at the coarsest level. One level per
        // emitter: the finest any of its particles needs.
        LocalSize(64, 1, 1);

        #include <Common/ParticlePool.glslh>
        #include <Common/ParticleDrawLayout.glslh>
        #include <Common/ParticleState.glslh>

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

        struct ParticleMeshHistory
        {
            mat4  Transform; // the transform the particle was drawn with
            uvec4 Stamp;     // x = the view frame (u_Frame.x) it was drawn at, y = its pool slot's Id then
        };

        Buffer(6) History
        {
            ParticleMeshHistory u_History[];
        };

        ReadBuffer(7) PoolSlots
        {
            ParticleSlot u_PoolSlots[];
        };

        PushConstant PushConstants
        {
            uvec4 u_Range; // x = the alive half's offset in AliveList, y = the draw slot, z = submesh count, w = capacity
            vec4  u_Scale; // xyz = the row's Scale
            uvec4 u_Frame; // x = the view frame (ParticleRenderer::m_MeshFrame, from 1)
            vec4  u_Sphere; // the mesh's local bounding sphere: xyz centre, w radius (cm)
            vec4  u_Lens;   // xyz = the eye (cm), w = the projection's focal scale (max of |P00|, |P11|) / 2
            uvec4 u_Lod;    // x = levels, y = the level table's first word in Args, z = pass (0 instances, 1 LOD),
                            // w = |P23| as float bits (1 perspective, 0 orthographic)
        };

        // Args words after the records (ParticleRenderer::ResolveMesh): per level, per submesh {IndexCount,
        // IndexOffset}; then each level's screen size (float bits); then the reduced level.
        uint LodSizeWord( uint level ) { return u_Lod.y + u_Lod.x * u_Range.z * 2u + level; }
        uint LodReduceWord() { return u_Lod.y + u_Lod.x * u_Range.z * 2u + u_Lod.x; }

        // Geometry::BoundsScreenRadiusSquared + SelectLODFromBounds (UE ComputeBoundsScreenRadiusSquared,
        // ComputeStaticMeshLOD): the coarsest level whose screen size the sphere is still below.
        uint ParticleMeshLevel( mat4 world, vec3 scale )
        {
            vec3  centre = ( world * vec4( u_Sphere.xyz, 1.0 ) ).xyz;
            float radius = u_Sphere.w * max( abs( scale.x ), max( abs( scale.y ), abs( scale.z ) ) );
            vec3  d      = centre - u_Lens.xyz;
            float distSq = dot( d, d ) * uintBitsToFloat( u_Lod.w );
            float r      = u_Lens.w * radius;
            float screenRadiusSq = r * r / max( 1.0, distSq );
            for ( uint level = u_Lod.x - 1u; level > 0u; --level )
            {
                float halfSize = 0.5 * uintBitsToFloat( u_Args[LodSizeWord( level )] );
                if ( halfSize * halfSize > screenRadiusSq )
                    return level;
            }
            return 0u;
        }

        void main()
        {
            uint alive = min( u_Slots[u_Range.y].VertexCount / 6u, u_Range.w );
            uint t     = gl_GlobalInvocationID.x;
            if ( u_Lod.z == 1u )
            {
                if ( t != 0u )
                    return;
                uint level = min( u_Args[LodReduceWord()], u_Lod.x - 1u );
                for ( uint s = 0u; s < u_Range.z; ++s )
                {
                    u_Args[4u + s * 5u + 0u] = u_Args[u_Lod.y + ( level * u_Range.z + s ) * 2u];
                    u_Args[4u + s * 5u + 2u] = u_Args[u_Lod.y + ( level * u_Range.z + s ) * 2u + 1u];
                }
                u_Args[LodReduceWord()] = u_Lod.x - 1u;
                return;
            }
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
            uint pool  = u_Alive[u_Range.x + t];
            uint n     = ParticleLocalIndex( pool );
            vec4 q     = ParticleMeshOrientation( n );
            float len  = length( q );
            q          = len > 0.0 ? q / len : vec4( 0.0, 0.0, 0.0, 1.0 );
            vec3 scale = ParticleMeshScale( n ) * u_Scale.xyz;
            // The quaternion's rotation matrix, column by column (x, y, z axes).
            vec3 x = vec3( 1.0 - 2.0 * ( q.y * q.y + q.z * q.z ), 2.0 * ( q.x * q.y + q.w * q.z ), 2.0 * ( q.x * q.z - q.w * q.y ) );
            vec3 y = vec3( 2.0 * ( q.x * q.y - q.w * q.z ), 1.0 - 2.0 * ( q.x * q.x + q.z * q.z ), 2.0 * ( q.y * q.z + q.w * q.x ) );
            vec3 z = vec3( 2.0 * ( q.x * q.z + q.w * q.y ), 2.0 * ( q.y * q.z - q.w * q.x ), 1.0 - 2.0 * ( q.x * q.x + q.y * q.y ) );
            const mat4 world = mat4( vec4( x * scale.x, 0.0 ), vec4( y * scale.y, 0.0 ), vec4( z * scale.z, 0.0 ),
                                     vec4( ParticleWorldPosition( n ), 1.0 ) );
            u_Instances[t] = world;
            if ( u_Lod.x > 1u )
                atomicMin( u_Args[LodReduceWord()], ParticleMeshLevel( world, scale ) );

            const uint                id   = u_PoolSlots[pool].Id;
            const ParticleMeshHistory last = u_History[n];
            const bool same = last.Stamp.x + 1u == u_Frame.x && last.Stamp.y == id;
            u_Instances[u_Range.w + t] = same ? last.Transform : world;
            u_History[n].Transform     = world;
            u_History[n].Stamp         = uvec4( u_Frame.x, id, 0u, 0u );
        }
    }
}
