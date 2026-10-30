// DesertAsset {"Kind":"Shader","Guid":"2885a34471dc4b45946b8f7be0dc8702","Versions":{"SHDR":1},"Dependencies":[]}
Shader "InstanceCull"
{
    // "GPUDriven: Cull ..." (Engine/Graphic/GpuDriven/InstanceCulling.cpp; UE 5.8 Renderer/Private/InstanceCulling/
    // InstanceCullingContext.cpp + Shaders/Private/InstanceCulling/BuildInstanceDrawCommands.usf, the two-pass HZB
    // occlusion of Shaders/Private/HZBOcclusion.usf and VirtualShadowMaps/VirtualShadowMapBuildPerPageDrawCommands.usf):
    // which instances each (view x mesh pass) draws, decided on the GPU, and the bins' indirect records.
    //
    // One program, four steps (Params.x):
    //  0 INIT DRAWS   one thread per (view, bin): the bin's VkDrawIndexedIndirectCommand with InstanceCount 0 and
    //                 firstInstance = the view's base + the bin's FirstVisible.
    //  1 MAIN         one thread per (view, instance): shadow-caster filter, ISM distance fade, frustum planes (and the
    //                 VSM level's drawn-page mask), LOD (the twin of Geometry::SelectLODFromBounds), then the
    //                 PREVIOUS frame's furthest HZB through PrevViewProjection. Survivors go into their LOD's bins;
    //                 the HZB-rejected are appended to Occluded for step 2. Writes Visibility[view, instance].
    //                 A caster view's swaying instance past the shadow wind distance (GpuCull_CastsRestPose) stands
    //                 with the still casters (a cached cascade's static layer); its entries carry
    //                 DESERT_GPU_VISIBLE_REST_POSE, and the vertex stage draws its rest pose.
    //                 VIRTUAL GEOMETRY (Params3.x != 0): an instance past the frustum test whose primitive has DAG
    //                 rows (VgPrimitiveDags) in a view with a cluster-cut view (VgCutViewOf) appends one
    //                 VgClusterCandidate per DAG row, compacted into its cut view's own region (rows cutView x
    //                 Params3.y, count VgCandidateCounts[cutView]), BEFORE the HZB test - the cluster cut owns VG
    //                 occlusion -
    //                 and goes into NO bin of that view (the VG raster draws its clusters; MeshRenderer's
    //                 VirtualGeometryIssue): it is not held back for step 2 either. A VG instance that still reaches
    //                 a bin of a view with a cut view counts into VgBinnedInstances (must stay 0; the cut's Status
    //                 carries it to DesertCtl `state views.virtualGeometry.binnedVgInstances`).
    //  2 POST         one thread per occluded entry (indirect, sized by step 3): this frame's furthest HZB through
    //                 ViewProjection; the newly visible go into this phase's bins.
    //  3 ARGS         one thread: the occluded count -> step 2's VkDispatchIndirectCommand.
    //
    // Every rule below is the CPU rule it replaces, spelled the same way, so the debug readback
    // (InstanceCullValidation) can demand the same set: Core::Frustum::Intersects (positive vertex, a zero plane
    // accepts), Geometry::TransformBounds (8 corners), Graphic::KeepsInstanceAtDistance + ScaledByLens,
    // WindExpandedBounds (horizontal pad), Geometry::SelectLODFromBounds + BoundsScreenRadiusSquared, and the
    // "VSM: Cull Casters" page test.
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/GpuScene.glslh>
        #include <Common/VirtualShadowMap.glslh>
        #include <Common/VirtualGeometry.glslh>
        #include <Common/GpuCullTests.glslh>

        ReadBuffer(0) GpuCullPrimitives
        {
            GpuScenePrimitive Primitives[];
        };
        ReadBuffer(1) GpuCullInstances
        {
            GpuSceneInstance Instances[];
        };
        ReadBuffer(2) GpuCullLodScreenSizes
        {
            float LodScreenSizes[];
        };
        ReadBuffer(3) GpuCullWorldBounds
        {
            GpuSceneWorldBounds WorldBounds[];
        };
        ReadBuffer(4) GpuCullViews
        {
            GpuCullView Views[];
        };
        ReadBuffer(5) GpuCullPassPrimitiveDraws
        {
            GpuPrimitivePassDraws PrimitiveDraws[];
        };
        ReadBuffer(6) GpuCullPassDrawSlots
        {
            GpuDrawSlot DrawSlots[];
        };
        ReadBuffer(7) GpuCullPassBins
        {
            GpuDrawBin Bins[];
        };
        Buffer(8) GpuCullIndirectDraws
        {
            GpuIndirectDraw Draws[];
        };
        Buffer(9) GpuCullVisibleInstances
        {
            GpuVisibleInstance Visible[];
        };
        Buffer(10) GpuCullOccluded
        {
            GpuOccludedInstance Occluded[];
        };
        Buffer(11) GpuCullOccludedCount
        {
            uint OccludedCount;
            uint OccludedCapacity;
            uint OccludedPad0;
            uint OccludedPad1;
        };
        Buffer(12) GpuCullDispatchArgs
        {
            uint DispatchX;
            uint DispatchY;
            uint DispatchZ;
            uint DispatchPad;
        };
        Buffer(13) GpuCullVisibility
        {
            uint Visibility[]; // [ViewIndex * instance rows + instance], DESERT_GPU_CULL_VISIBILITY_*
        };
        ReadBuffer(14) GpuCullVsmDirtyMask
        {
            uint DirtyPageMask[];
        };

        // The binned draws' per-instance vertex stream (4 B per visible instance): the GpuScene instance of
        // Visible[i], at the same index, so a draw's gl_InstanceIndex reaches its instance row without a second
        // indirection.
        Buffer(16) GpuCullVisibleInstanceIds
        {
            uint VisibleInstanceIds[];
        };

        // THE VG CANDIDATE APPEND (GpuDriven::VirtualGeometryCandidateInput / Output).
        ReadBuffer(17) GpuCullVgPrimitiveDags
        {
            uvec2 VgPrimitiveDags[]; // per primitive row: (first VgDagEntries row, DAG count)
        };
        ReadBuffer(18) GpuCullVgDagEntries
        {
            uvec2 VgDagEntries[]; // (Dags[] row, its RootNode)
        };
        ReadBuffer(19) GpuCullVgCutViewOf
        {
            uint VgCutViewOf[]; // by the caller's view index: the candidate's View, DESERT_GPU_INVALID_INDEX = none
        };
        Buffer(20) GpuCullVgCandidates
        {
            VgClusterCandidate VgCandidates[];
        };
        Buffer(21) GpuCullVgCandidateCount
        {
            uint VgBinnedInstances;   // VG instances binned in a view with a cut view (an invariant: 0)
            uint VgCandidateCounts[]; // per cut view: rows appended to its region (may pass Params3.y: the cut clamps)
        };

        Uniform sampler PointClampSampler;
        Uniform(15) texture2D GpuCullHzbTexture; // the furthest HZB of the dispatch's HZB view (last frame's in step 1)

        PushConstant PushConstants
        {
            uvec4 Params;  // x = step, y = instance rows, z = views of this dispatch, w = first view in Views[]
            uvec4 Params2; // x = bins of the pass, y = visible capacity per view, z = primitive rows of the pass
                           // table, w = 1 when a HZB is bound
            uvec4 Params3; // x = VG primitive rows (0 = no candidate append), y = VG candidate rows per cut view
        };

        uint ThreadIndex()
        {
            return ( gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x ) * 64u + gl_LocalInvocationIndex;
        }

        // "VSM: Cull Casters"' page test (Common/VirtualShadowMap.glslh VsmBoxMaskCells), against this view's level.
        bool CoversDrawnVsmCell( GpuCullView view, vec3 boxMin, vec3 boxMax )
        {
            ivec2 lowCell;
            ivec2 highCell;
            if ( !VsmBoxMaskCells( view.ViewProjection, boxMin, boxMax, lowCell, highCell ) )
                return false;
            for ( int y = lowCell.y; y <= highCell.y; ++y )
            {
                for ( int x = lowCell.x; x <= highCell.x; ++x )
                {
                    const uint index = uint( y ) * VSM_MASK_CELLS + uint( x );
                    if ( ( DirtyPageMask[view.VsmLevel * VSM_MASK_WORDS_PER_LEVEL + index / 32u] & ( 1u << ( index % 32u ) ) ) != 0u )
                        return true;
                }
            }
            return false;
        }

        // Graphic::InstanceFadeThreshold (InstanceCullDistance.hpp): PCG output permutation of the index.
        float InstanceFadeThreshold( uint index )
        {
            uint state = index * 747796405u + 2891336453u;
            uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
            word       = ( word >> 22u ) ^ word;
            return float( word >> 8u ) * ( 1.0 / 16777216.0 );
        }

        // Graphic::KeepsInstanceAtDistance with ScaledByLens( cull, DrawDistanceScale( main view ) ).
        bool KeepsInstanceAtDistance( GpuCullView view, GpuSceneInstance instance, uint index, vec3 position )
        {
            const float scale = view.CullOrigin.w;
            const float minD  = instance.CullDistanceStart * scale;
            const float maxD  = instance.CullDistanceEnd * scale;
            if ( !( maxD > 0.0 ) )
                return true;
            const vec3  offset     = position - view.CullOrigin.xyz;
            const float distanceSq = dot( offset, offset );
            if ( distanceSq >= maxD * maxD )
                return false;
            if ( distanceSq <= minD * minD )
                return true;
            const float fade = ( sqrt( distanceSq ) - minD ) / ( maxD - minD );
            return InstanceFadeThreshold( index ) >= fade;
        }

        // Geometry::SelectLODFromBounds + BoundsScreenRadiusSquared (LODSelection.hpp), after MeshRenderer::ComputeLOD's
        // order (forced level first, then the renderer's LOD toggle).
        uint SelectLod( GpuCullView view, GpuScenePrimitive primitive, mat4 world, bool emptyBounds )
        {
            if ( primitive.ForcedLod >= 0 )
                return uint( primitive.ForcedLod );
            if ( ( view.Flags & DESERT_GPU_CULL_VIEW_LOD_DISABLED ) != 0u || primitive.LodCount <= 1u || emptyBounds ||
                 primitive.FirstLodScreenSize == DESERT_GPU_INVALID_INDEX )
                return 0u;
            const float scale     = max( length( world[0].xyz ), max( length( world[1].xyz ), length( world[2].xyz ) ) );
            const vec3  extent    = primitive.LocalBoundsExtent.xyz;
            const vec3  boundsMin = primitive.LocalBoundsCenter.xyz - extent;
            const vec3  boundsMax = primitive.LocalBoundsCenter.xyz + extent;
            const float radius    = length( boundsMax - boundsMin ) * 0.5 * scale;
            const vec3  center    = ( world * vec4( 0.5 * ( boundsMin + boundsMax ), 1.0 ) ).xyz;

            const vec3  d        = center - view.LodOrigin.xyz;
            const float distSq   = dot( d, d ) * abs( view.LodProjection[2][3] );
            const float multiple = max( 0.5 * abs( view.LodProjection[0][0] ), 0.5 * abs( view.LodProjection[1][1] ) );
            const float screenRadiusSq = ( multiple * radius ) * ( multiple * radius ) / max( 1.0, distSq );

            const int coarsest = int( primitive.LodCount ) - 1;
            int       base     = 0;
            for ( int level = coarsest; level > 0; --level )
            {
                const float halfSize = 0.5 * LodScreenSizes[primitive.FirstLodScreenSize + uint( level )];
                if ( halfSize * halfSize > screenRadiusSq )
                {
                    base = level;
                    break;
                }
            }
            return uint( clamp( base + primitive.LodBias, 0, coarsest ) );
        }

        // The instance's world box (Geometry::TransformBounds of the local box, 8 corners, widened by the wind; or
        // its pose box). Returns true when the primitive has no extent at all (IsVisibleInView: "I do not know where
        // this is" = draw).
        bool WorldBox( GpuScenePrimitive primitive, GpuSceneInstance instance, mat4 world, out vec3 boxMin,
                       out vec3 boxMax )
        {
            if ( ( primitive.Flags & DESERT_GPU_PRIMITIVE_WORLD_BOUNDS ) != 0u &&
                 instance.WorldBoundsRow != DESERT_GPU_INVALID_INDEX )
            {
                boxMin = WorldBounds[instance.WorldBoundsRow].Min.xyz;
                boxMax = WorldBounds[instance.WorldBoundsRow].Max.xyz;
                return false;
            }
            const vec3 extent = primitive.LocalBoundsExtent.xyz;
            const vec3 centre = primitive.LocalBoundsCenter.xyz;
            // GpuScene keeps centre = extent = 0 for a mesh with no bounds (GpuSceneRegistry FillBounds).
            if ( all( equal( extent, vec3( 0.0 ) ) ) && all( equal( centre, vec3( 0.0 ) ) ) )
            {
                boxMin = vec3( 0.0 );
                boxMax = vec3( 0.0 );
                return true;
            }
            const vec3 localMin = centre - extent;
            const vec3 localMax = centre + extent;
            boxMin              = vec3( 3.0e38 );
            boxMax              = vec3( -3.0e38 );
            for ( uint c = 0u; c < 8u; ++c )
            {
                const vec3 corner = vec3( ( c & 1u ) != 0u ? localMax.x : localMin.x, ( c & 2u ) != 0u ? localMax.y : localMin.y,
                                          ( c & 4u ) != 0u ? localMax.z : localMin.z );
                const vec3 w = ( world * vec4( corner, 1.0 ) ).xyz;
                boxMin       = min( boxMin, w );
                boxMax       = max( boxMax, w );
            }
            // WindExpandedBounds: the wind moves vertices horizontally by up to the row's pad.
            const float pad = primitive.LocalBoundsCenter.w;
            boxMin -= vec3( pad, 0.0, pad );
            boxMax += vec3( pad, 0.0, pad );
            return false;
        }

        // Counts the instance into each submesh bin of its LOD in the pass and writes its visible entries; @p restPose
        // (GpuCull_CastsRestPose) rides each entry's DESERT_GPU_VISIBLE_REST_POSE bit to the vertex stage.
        void EmitToBins( uint localView, uint instanceRow, uint primitiveRow, uint lod, bool restPose )
        {
            if ( primitiveRow >= Params2.z )
                return;
            const GpuPrimitivePassDraws pd = PrimitiveDraws[primitiveRow];
            if ( pd.SubmeshCount == 0u || pd.LodCount == 0u )
                return;
            if ( Params3.x != 0u && primitiveRow < Params3.x && VgPrimitiveDags[primitiveRow].y != 0u &&
                 VgCutViewOf[Views[Params.w + localView].ViewIndex] != DESERT_GPU_INVALID_INDEX )
                atomicAdd( VgBinnedInstances, 1u );
            const uint level = min( lod, pd.LodCount - 1u );
            for ( uint s = 0u; s < pd.SubmeshCount; ++s )
            {
                const uint slot = pd.FirstDrawSlot + level * pd.SubmeshCount + s;
                const uint bin  = DrawSlots[slot].Bin;
                const uint k    = atomicAdd( Draws[localView * Params2.x + bin].InstanceCount, 1u );
                const uint entry = localView * Params2.y + Bins[bin].FirstVisible + k;
                Visible[entry] =
                     GpuVisibleInstance( instanceRow, restPose ? slot | DESERT_GPU_VISIBLE_REST_POSE : slot );
                VisibleInstanceIds[entry] = instanceRow;
            }
        }

        // One VgClusterCandidate per DAG row of the instance's primitive, for the view's cluster-cut view. True when
        // the instance is the cut's in this view (appended, even past the capacity): it is drawn by the VG raster,
        // never by the view's bins.
        bool AppendVgCandidates( uint viewIndex, uint instanceRow, uint primitiveRow )
        {
            if ( primitiveRow >= Params3.x )
                return false;
            const uint cutView = VgCutViewOf[viewIndex];
            if ( cutView == DESERT_GPU_INVALID_INDEX )
                return false;
            const uvec2 range = VgPrimitiveDags[primitiveRow];
            if ( range.y == 0u )
                return false;
            for ( uint k = 0u; k < range.y; ++k )
            {
                const uvec2 entry = VgDagEntries[range.x + k];
                const uint  slot  = atomicAdd( VgCandidateCounts[cutView], 1u );
                if ( slot < Params3.y ) // compacted per cut view: region cutView x Params3.y
                    VgCandidates[cutView * Params3.y + slot] = VgClusterCandidate( instanceRow, entry.x, entry.y, cutView );
            }
            return true;
        }

        void InitDraws()
        {
            const uint i = ThreadIndex();
            if ( i >= Params.z * Params2.x )
                return;
            const uint       localView = i / Params2.x;
            const uint       bin       = i - localView * Params2.x;
            const GpuDrawBin b         = Bins[bin];
            Draws[i] = GpuIndirectDraw( b.IndexCount, 0u, b.FirstIndex, b.VertexOffset, localView * Params2.y + b.FirstVisible,
                                        bin, 0u, 0u );
        }

        void CullMain()
        {
            const uint i = ThreadIndex();
            if ( i >= Params.y * Params.z )
                return;
            const uint        localView   = i / Params.y;
            const uint        instanceRow = i - localView * Params.y;
            const GpuCullView view        = Views[Params.w + localView];
            const uint        cell        = view.ViewIndex * Params.y + instanceRow;

            const GpuSceneInstance instance = Instances[instanceRow];
            if ( instance.Primitive == DESERT_GPU_INVALID_INDEX )
            {
                Visibility[cell] = 0u;
                return;
            }
            const GpuScenePrimitive primitive = Primitives[instance.Primitive];
            // A free instance row keeps its last primitive index; only the rows inside the primitive's range are live.
            if ( instanceRow < primitive.FirstInstance || instanceRow >= primitive.FirstInstance + primitive.InstanceCount ||
                 ( ( view.Flags & DESERT_GPU_CULL_VIEW_SHADOW_CASTER ) != 0u &&
                   ( primitive.Flags & DESERT_GPU_PRIMITIVE_CASTS_SHADOW ) == 0u ) )
            {
                Visibility[cell] = 0u;
                return;
            }
            const mat4 world = GpuScene_LocalToWorld( primitive ) * GpuScene_InstanceToPrimitive( instance );
            // A cached cascade's layer (GpuDriven::CullViewFlags::kCaster*Layer), per instance: the static layer
            // keeps what stands still (a swaying instance past the shadow wind distance among it, in its rest
            // pose), the movable layer what moves by itself.
            const bool restPose = GpuCull_CastsRestPose( view, primitive, world );
            const bool movable  = ( primitive.Flags & DESERT_GPU_PRIMITIVE_MOVABLE_CASTER ) != 0u && !restPose;
            if ( ( ( view.Flags & DESERT_GPU_CULL_VIEW_CASTER_STATIC_LAYER ) != 0u && movable ) ||
                 ( ( view.Flags & DESERT_GPU_CULL_VIEW_CASTER_MOVABLE_LAYER ) != 0u && !movable ) )
            {
                Visibility[cell] = 0u;
                return;
            }
            if ( !KeepsInstanceAtDistance( view, instance, instanceRow - primitive.FirstInstance, world[3].xyz ) )
            {
                Visibility[cell] = 0u;
                return;
            }
            vec3       boxMin;
            vec3       boxMax;
            const bool empty = WorldBox( primitive, instance, world, boxMin, boxMax );
            if ( !empty )
            {
                bool inside = GpuCull_FrustumIntersects( view, boxMin, boxMax );
                if ( inside && ( view.Flags & DESERT_GPU_CULL_VIEW_VSM_PAGE_MASK ) != 0u )
                    inside = CoversDrawnVsmCell( view, boxMin, boxMax );
                if ( !inside )
                {
                    Visibility[cell] = 0u;
                    return;
                }
            }
            const uint lod  = SelectLod( view, primitive, world, empty );
            const uint word = DESERT_GPU_CULL_VISIBILITY_IN_VIEW | min( lod, DESERT_GPU_CULL_VISIBILITY_LOD_MASK );
            if ( Params3.x != 0u && AppendVgCandidates( view.ViewIndex, instanceRow, instance.Primitive ) )
            {
                Visibility[cell] = word; // in view; the cluster cut tests its occlusion per node / cluster
                return;
            }

            if ( !empty && Params2.w != 0u && ( view.Flags & DESERT_GPU_CULL_VIEW_TEST_HZB ) != 0u &&
                 ( primitive.Flags & DESERT_GPU_PRIMITIVE_SKIP_OCCLUSION ) == 0u &&
                 GpuCull_IsOccludedByHzb( GpuCullHzbTexture, PointClampSampler, view, view.PrevViewProjection, boxMin, boxMax ) )
            {
                const uint slot = atomicAdd( OccludedCount, 1u );
                if ( slot < OccludedCapacity )
                    Occluded[slot] = GpuOccludedInstance( instanceRow, view.ViewIndex );
                Visibility[cell] = word | DESERT_GPU_CULL_VISIBILITY_OCCLUDED;
                return;
            }
            Visibility[cell] = word;
            EmitToBins( localView, instanceRow, instance.Primitive, lod, restPose );
        }

        void CullPostOcclusion()
        {
            const uint i = ThreadIndex();
            if ( i >= min( OccludedCount, OccludedCapacity ) )
                return;
            const GpuOccludedInstance entry     = Occluded[i];
            uint                      localView = 0xFFFFFFFFu;
            for ( uint v = 0u; v < Params.z; ++v )
                if ( Views[Params.w + v].ViewIndex == entry.View )
                    localView = v;
            if ( localView == 0xFFFFFFFFu )
                return; // a view of another mesh pass' dispatch
            const GpuCullView       view      = Views[Params.w + localView];
            const GpuSceneInstance  instance  = Instances[entry.Instance];
            const GpuScenePrimitive primitive = Primitives[instance.Primitive];
            const mat4              world     = GpuScene_LocalToWorld( primitive ) * GpuScene_InstanceToPrimitive( instance );
            vec3                    boxMin;
            vec3                    boxMax;
            WorldBox( primitive, instance, world, boxMin, boxMax );
            if ( Params2.w != 0u && GpuCull_IsOccludedByHzb( GpuCullHzbTexture, PointClampSampler, view, view.ViewProjection, boxMin, boxMax ) )
                return;
            const uint cell = entry.View * Params.y + entry.Instance;
            const uint lod  = Visibility[cell] & DESERT_GPU_CULL_VISIBILITY_LOD_MASK;
            atomicOr( Visibility[cell], DESERT_GPU_CULL_VISIBILITY_POST_VISIBLE );
            EmitToBins( localView, entry.Instance, instance.Primitive, lod,
                        GpuCull_CastsRestPose( view, primitive, world ) );
        }

        void WriteDispatchArgs()
        {
            if ( ThreadIndex() != 0u )
                return;
            const uint groups = ( min( OccludedCount, OccludedCapacity ) + 63u ) / 64u;
            DispatchX         = min( groups, 65535u );
            DispatchY         = groups == 0u ? 1u : ( groups + 65534u ) / 65535u;
            DispatchZ         = 1u;
            DispatchPad       = 0u;
        }

        void main()
        {
            if ( Params.x == 0u )
                InitDraws();
            else if ( Params.x == 1u )
                CullMain();
            else if ( Params.x == 2u )
                CullPostOcclusion();
            else
                WriteDispatchArgs();
        }
    }
}
