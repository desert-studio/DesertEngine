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
        // Motion (UE NiagaraMeshVertexFactory's PrevPosition / PrevScale / PrevRotation): InstanceTransforms[capacity + t]
        // is the t-th instance's transform at the previous view frame, from the particle's History row (indexed by
        // its range-local slot, which the alive list keeps across frames). The row counts only if it was written at
        // the previous frame (Frame.x - 1) and by the same particle: the row keeps the pool slot's Id, which
        // every spawn writes anew (ParticleSimulate IdBase + t, UE Niagara's persistent particle ID), so a slot
        // freed and respawned between two frames never reads the dead particle's row - with or without an Age
        // attribute. Otherwise (just spawned, or not drawn last frame) the previous transform is this frame's: no
        // motion. Mesh/Surface/Vertex_Instanced.glslh reads it at the push's previous-instance
        // offset (= capacity).
        // LOD (UE Niagara Mesh Renderer, a level by screen size; Geometry::SelectLODFromBounds is the same rule
        // for a mesh component): Lod.z = 0 - each live particle measures its world bounding sphere (the mesh's
        // local sphere through its transform) on screen and atomicMin's the coarsest level it is still below into
        // the reduction word; Lod.z = 1 (one thread, the next pass) - copies that level's index ranges from
        // the level table into every submesh record and re-arms the word at the coarsest level. One level per
        // emitter: the finest any of its particles needs.
        LocalSize(64, 1, 1);

        #include <Common/ParticlePool.glslh>
        #include <Common/ParticleDrawLayout.glslh>
        #include <Common/ParticleState.glslh>

        ReadBuffer(0) AttributeFloats
        {
            float DrawFloats[];
        };

        ReadBuffer(5) DrawLayout
        {
            ParticleDrawLayout DrawLayoutData;
        };

        #include <Common/ParticleDrawRead.glslh>

        ReadBuffer(1) AliveList
        {
            uint AliveData[];
        };

        ReadBuffer(2) Counters
        {
            ParticleDrawSlot Slots[];
        };

        Buffer(3) Instances
        {
            mat4 InstanceTransforms[];
        };

        Buffer(4) Args
        {
            uint Arguments[]; // GpuInstances.hpp: header, then 5 words per submesh record
        };

        struct ParticleMeshHistory
        {
            mat4  Transform; // the transform the particle was drawn with
            uvec4 Stamp;     // x = the view frame (Frame.x) it was drawn at, y = its pool slot's Id then
        };

        Buffer(6) History
        {
            ParticleMeshHistory HistoryEntries[];
        };

        ReadBuffer(7) PoolSlots
        {
            ParticleSlot PoolSlotsData[];
        };

        PushConstant PushConstants
        {
            uvec4 SortRange; // x = the alive half's offset in AliveList, y = the draw slot, z = submesh count, w = capacity
            vec4  Scale; // xyz = the row's Scale
            uvec4 Frame; // x = the view frame (ParticleRenderer::m_MeshFrame, from 1)
            vec4  BoundingSphere; // the mesh's local bounding sphere: xyz centre, w radius (cm)
            vec4  Lens;   // xyz = the eye (cm), w = the projection's focal scale (max of |P00|, |P11|) / 2
            uvec4 Lod;    // x = levels, y = the level table's first word in Args, z = pass (0 instances, 1 LOD),
                            // w = |P23| as float bits (1 perspective, 0 orthographic)
        };

        // Args words after the records (ParticleRenderer::ResolveMesh): per level, per submesh {IndexCount,
        // IndexOffset}; then each level's screen size (float bits); then the reduced level.
        uint LodSizeWord( uint level ) { return Lod.y + Lod.x * SortRange.z * 2u + level; }
        uint LodReduceWord() { return Lod.y + Lod.x * SortRange.z * 2u + Lod.x; }

        // Geometry::BoundsScreenRadiusSquared + SelectLODFromBounds (UE ComputeBoundsScreenRadiusSquared,
        // ComputeStaticMeshLOD): the coarsest level whose screen size the sphere is still below.
        uint ParticleMeshLevel( mat4 world, vec3 scale )
        {
            vec3  centre = ( world * vec4( BoundingSphere.xyz, 1.0 ) ).xyz;
            float radius = BoundingSphere.w * max( abs( scale.x ), max( abs( scale.y ), abs( scale.z ) ) );
            vec3  d      = centre - Lens.xyz;
            float distSq = dot( d, d ) * uintBitsToFloat( Lod.w );
            float r      = Lens.w * radius;
            float screenRadiusSq = r * r / max( 1.0, distSq );
            for ( uint level = Lod.x - 1u; level > 0u; --level )
            {
                float halfSize = 0.5 * uintBitsToFloat( Arguments[LodSizeWord( level )] );
                if ( halfSize * halfSize > screenRadiusSq )
                    return level;
            }
            return 0u;
        }

        void main()
        {
            uint alive = min( Slots[SortRange.y].VertexCount / 6u, SortRange.w );
            uint t     = gl_GlobalInvocationID.x;
            if ( Lod.z == 1u )
            {
                if ( t != 0u )
                    return;
                uint level = min( Arguments[LodReduceWord()], Lod.x - 1u );
                for ( uint s = 0u; s < SortRange.z; ++s )
                {
                    Arguments[4u + s * 5u + 0u] = Arguments[Lod.y + ( level * SortRange.z + s ) * 2u];
                    Arguments[4u + s * 5u + 2u] = Arguments[Lod.y + ( level * SortRange.z + s ) * 2u + 1u];
                }
                Arguments[LodReduceWord()] = Lod.x - 1u;
                return;
            }
            if ( t == 0u )
            {
                for ( uint s = 0u; s < SortRange.z; ++s )
                {
                    Arguments[4u + s * 5u + 1u] = alive;
                    Arguments[4u + s * 5u + 4u] = 0u;
                }
                Arguments[3] = alive;
            }
            if ( t >= alive )
                return;
            uint pool  = AliveData[SortRange.x + t];
            uint n     = ParticleLocalIndex( pool );
            vec4 q     = ParticleMeshOrientation( n );
            float len  = length( q );
            q          = len > 0.0 ? q / len : vec4( 0.0, 0.0, 0.0, 1.0 );
            vec3 scale = ParticleMeshScale( n ) * Scale.xyz;
            // The quaternion's rotation matrix, column by column (x, y, z axes).
            vec3 x = vec3( 1.0 - 2.0 * ( q.y * q.y + q.z * q.z ), 2.0 * ( q.x * q.y + q.w * q.z ), 2.0 * ( q.x * q.z - q.w * q.y ) );
            vec3 y = vec3( 2.0 * ( q.x * q.y - q.w * q.z ), 1.0 - 2.0 * ( q.x * q.x + q.z * q.z ), 2.0 * ( q.y * q.z + q.w * q.x ) );
            vec3 z = vec3( 2.0 * ( q.x * q.z + q.w * q.y ), 2.0 * ( q.y * q.z - q.w * q.x ), 1.0 - 2.0 * ( q.x * q.x + q.y * q.y ) );
            // A Local emitter's mesh goes through the entity's full transform as a whole - its position, its
            // MeshOrientation and its Scale are the placement frame's (UE: the local-space LocalToWorld of the mesh
            // vertex factory); a World emitter's frame is the identity.
            const uint pc = DrawLayoutData.Columns0.x;
            const vec3 p  = vec3( ParticleFloat( n, pc, 0u, 0.0 ), ParticleFloat( n, pc, 1u, 0.0 ),
                                  ParticleFloat( n, pc, 2u, 0.0 ) );
            const mat4 world = ParticleSimulationToWorld() *
                               mat4( vec4( x * scale.x, 0.0 ), vec4( y * scale.y, 0.0 ), vec4( z * scale.z, 0.0 ),
                                     vec4( p, 1.0 ) );
            InstanceTransforms[t] = world;
            if ( Lod.x > 1u )
                // The instance's world scale per axis (its columns' lengths): the particle's Scale, and for a Local
                // emitter the entity's too.
                atomicMin( Arguments[LodReduceWord()],
                           ParticleMeshLevel( world, vec3( length( world[0].xyz ), length( world[1].xyz ),
                                                           length( world[2].xyz ) ) ) );

            const uint                id   = PoolSlotsData[pool].Id;
            const ParticleMeshHistory last = HistoryEntries[n];
            const bool same = last.Stamp.x + 1u == Frame.x && last.Stamp.y == id;
            InstanceTransforms[SortRange.w + t] = same ? last.Transform : world;
            HistoryEntries[n].Transform     = world;
            HistoryEntries[n].Stamp         = uvec4( Frame.x, id, 0u, 0u );
        }
    }
}
