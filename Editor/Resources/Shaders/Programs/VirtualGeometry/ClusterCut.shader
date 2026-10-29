// DesertAsset {"Kind":"Shader","Guid":"160b36d01f8843f389a49ec2291e7d32","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ClusterCut"
{
    // "VirtualGeometry: Cluster Cut" (Engine/Graphic/VirtualGeometry/VirtualGeometryRenderer.cpp AddClusterCull;
    // UE 5.8 Shaders/Private/Nanite/NaniteClusterCulling.usf NodeAndClusterCull + NaniteHierarchyTraversal.ush):
    // which clusters of each VG candidate (instance x submesh DAG x view, appended by InstanceCulling) are drawn.
    //
    // One program, two steps (Params.x):
    //  0 ARGS  one thread: the counts of the dispatch's own regions -> step 1's VkDispatchIndirectCommand. Main
    //          (candidates compacted PER CUT VIEW by InstanceCull: region v = Candidates[v x Params.w, + count v]):
    //          X/Y = the largest of its views' counts, Z = its views - no workgroup for another view's candidate.
    //          Post (a main cut's carry: region 0 node children, region 1 clusters): one workgroup per child, then
    //          one per 64 clusters (UE batches the post pass's clusters: no one-cluster workgroup).
    //  1 CUT   one workgroup per candidate: the DAG's BVH from the candidate's Node, breadth first through two
    //          shared ping-pong queues (kQueueCapacity); a node that does not fit continues depth first on the
    //          thread's own stack (kStackDepth). A child is visited when its box meets the frustum AND its
    //          MaxParentLodError projects past tau (a parent below may be too coarse). A leaf child names one
    //          group: each of its clusters is drawn when its box meets the frustum AND its own LodError projects
    //          to tau or less. Both tests call VgProjectedError on bit-identical (sphere, error) pairs, so a group
    //          and the clusters generated from it never both draw nor both hide (no crack).
    //          Drawn clusters are HW clusters (no SW raster yet): VisibleClusters[capacity - 1 - i], counted in
    //          RasterArgs.HwDraw[1] (GpuClusterRasterArgs, the UE layout: SW from the front, HW from the back).
    //          A dispatch serves the cut views [Params3.x, Params3.x + Params3.y) of one mesh pass (the main view;
    //          one shadow cascade), workgroup Z = the view. LOD in the view's own pixels (ViewLods.z = 1: an
    //          orthographic shadow cascade, lodScale = its texels per cm).
    //
    // OCCLUSION: the instance cull appends VG candidates before its HZB test, so the cut owns VG occlusion. A node
    // child and a cluster past the frustum test are tested against the PREVIOUS frame's furthest HZB through
    // PrevViewProjection with the instance cull's own test (Common/GpuCullTests.glslh GpuCull_IsOccludedByHzb, one
    // source), when a HZB is bound (Params2.x), the view tests it (DESERT_GPU_CULL_VIEW_TEST_HZB) and the primitive
    // does not skip occlusion. The occluded are counted in Status (OccludedNodes / OccludedClusters) and, in the
    // MAIN phase with a carry (Params2.y = its capacity per region), appended to OccludedCandidates for the POST
    // phase (UE NaniteClusterCulling.usf: occluded nodes / clusters into the post pass's queues): an occluded node
    // child as VG_POST_CHILD | (node << 2 | k) in region 0, an occluded cluster as VG_POST_CLUSTER | cluster in
    // region 1 (rows Params2.y + i), in the candidate's Node.
    // The POST phase (Params2.z = 1) cuts those entries against THIS frame's furthest HZB through ViewProjection
    // (built from the main phase's depth, VG clusters included): a child is visited exactly as the main phase would
    // (LOD + box re-tested), a cluster re-tested alone. What the post phase finds occluded is hidden for good.
    //
    // Nothing is dropped silently: a stack that fills, a candidate whose primitive has no draw slot for its DAG's
    // submesh in the view's pass, and a visible list past its capacity each count in Status with the lowest DAG
    // row, and the host names the mesh (ClusterCutCounts).
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/GpuScene.glslh>
        #include <Common/VirtualGeometry.glslh>
        #include <Common/GpuCullTests.glslh>
        #include <Common/VirtualShadowMap.glslh>

        ReadBuffer(0) ClusterCutPrimitives
        {
            GpuScenePrimitive Primitives[];
        };
        ReadBuffer(1) ClusterCutInstances
        {
            GpuSceneInstance Instances[];
        };
        ReadBuffer(2) ClusterCutPassPrimitiveDraws
        {
            GpuPrimitivePassDraws PrimitiveDraws[];
        };
        ReadBuffer(3) ClusterCutPassDrawSlots
        {
            GpuDrawSlot DrawSlots[];
        };
        ReadBuffer(4) ClusterCutSubmeshTransforms
        {
            mat4 SubmeshTransforms[];
        };
        ReadBuffer(5) ClusterCutDags
        {
            VgClusterDag Dags[];
        };
        ReadBuffer(6) ClusterCutClusters
        {
            VgCluster Clusters[];
        };
        ReadBuffer(7) ClusterCutHierarchy
        {
            VgHierarchyNode Nodes[];
        };
        ReadBuffer(8) ClusterCutCandidates
        {
            VgClusterCandidate Candidates[];
        };
        ReadBuffer(9) ClusterCutViews
        {
            GpuCullView Views[];
        };
        ReadBuffer(10) ClusterCutViewLods
        {
            vec4 ViewLods[]; // per view: x = lodScale, y = tau (pixels per edge), z = 1 orthographic (ClusterLodMeasure)
        };
        ReadBuffer(11) ClusterCutCandidateCount
        {
            uint CandidateBinnedVg; // InstanceCull's VgBinnedInstances (0 = no VG instance in the view's bins)
            uint CandidateCounts[]; // per region (may pass Params.w: clamped). Main: region = cut view; post:
                                    // [0] carried node children, [1] carried clusters
        };
        Buffer(12) ClusterCutVisibleClusters
        {
            VgVisibleCluster Visible[];
        };
        Buffer(13) ClusterCutRasterArgs
        {
            VgRasterArgs RasterArgs;
        };
        Buffer(14) ClusterCutStatus
        {
            uint StackOverflow;    // nodes a full stack could not take
            uint StackOverflowDag; // lowest DAG row among them (VG_INVALID_INDEX = none)
            uint MissingSlot;      // candidates whose primitive has no lod-0 draw slot for the DAG's submesh
            uint MissingSlotDag;
            uint VisibleOverflow;  // clusters past Params.y
            uint VisibleOverflowDag;
            uint OccludedNodes;    // node children the previous-frame HZB hid (their subtree not visited)
            uint OccludedClusters; // clusters of the cut the previous-frame HZB hid
            uint BinnedVgInstances; // copied from the candidates' count block (WriteDispatchArgs), for the readback
            uint OccludedOverflow;  // occluded entries past the carry's capacity (main phase; lost for one frame)
        };
        Buffer(15) ClusterCutDispatchArgs
        {
            uint DispatchX;
            uint DispatchY;
            uint DispatchZ;
            uint DispatchPad;
        };

        Buffer(17) ClusterCutOccludedCandidates
        {
            VgClusterCandidate OccludedCandidates[]; // main phase: the post phase's candidates (Node encoded), two
                                                     // regions of Params2.y rows: node children, clusters
        };
        Buffer(18) ClusterCutOccludedCount
        {
            uint OccludedBinnedVg; // stays 0
            uint OccludedCounts[]; // [0] children, [1] clusters: the post phase's CandidateCounts (same layout)
        };

        // A virtual shadow map level's view (DESERT_GPU_CULL_VIEW_VSM_PAGE_MASK): "VSM: Dirty Mask" of this frame,
        // kVsmMaskWordsPerLevel words per level (a one-word stand-in when the dispatch has no such view).
        ReadBuffer(19) ClusterCutVsmDirtyMask
        {
            uint DirtyPageMask[];
        };

        Uniform sampler PointClampSampler;
        Uniform(16) texture2D ClusterCutHzbTexture; // the previous frame's furthest HZB of the views (neutral: none)
        #define ClusterCutHzb sampler2D(ClusterCutHzbTexture, PointClampSampler)

        PushConstant PushConstants
        {
            uvec4 Params;  // x = step, y = visible capacity, z = primitive rows of the pass table, w = rows per region
            uvec4 Params2; // x = 1 when a HZB is bound, y = the occluded carry's rows per region (main; 0 = none),
                           // z = 1 in the post phase (this frame's HZB through ViewProjection), w = 0
            uvec4 Params3; // x = the dispatch's first cut view, y = its view count (Views[] / ViewLods[] rows)
        };

        #define VG_POST_CHILD   0x80000000u // post entry: child (Node & 3) of node (Node & VG_POST_PAYLOAD) >> 2
        #define VG_POST_CLUSTER 0x40000000u // post entry: one cluster row
        #define VG_POST_PAYLOAD 0x3FFFFFFFu

        #define kQueueCapacity 512u
        #define kStackDepth    48u

        shared uint s_Queue[2][kQueueCapacity];
        shared uint s_QueueCount[2];
        shared uint s_Occluded[2]; // the workgroup's occluded node children / clusters, added to Status once
        // The thread's own depth-first stack (per invocation, not shared).
        uint g_Stack[kStackDepth];
        uint g_Depth = 0u;

        // The candidate's frame: everything both tests need, the same for every thread of the workgroup.
        struct CutFrame
        {
            mat4  World;
            float WorldScale;
            vec3  Origin;
            float LodScale;
            float Tau;
            bool  Orthographic; // LOD in texels of an orthographic view (no distance)
            uint  Instance;
            uint  Dag;
            uint  View;
            bool  TestHzb; // a HZB is bound, the view tests it and the primitive does not skip occlusion
        };

        // Main phase with a carry: @p entry (VG_POST_CHILD / VG_POST_CLUSTER encoded) for the post phase.
        void CarryOccluded( CutFrame f, uint entry )
        {
            if ( Params2.y == 0u )
                return;
            const uint region = ( entry & VG_POST_CLUSTER ) != 0u ? 1u : 0u;
            const uint i      = atomicAdd( OccludedCounts[region], 1u );
            if ( i >= Params2.y )
            {
                atomicAdd( OccludedCounts[region], 0xFFFFFFFFu ); // net count stays the capacity
                atomicAdd( OccludedOverflow, 1u );
                return;
            }
            OccludedCandidates[region * Params2.y + i] = VgClusterCandidate( f.Instance, f.Dag, entry, f.View );
        }

        // UE Nanite's VSM page cull (NaniteCullRaster: a node / cluster is drawn into a virtual shadow map only over
        // pages it is rendered to this frame): a VSM level's box covers a "VSM: Dirty Mask" cell of its level holding
        // a page drawn this frame (Common/VirtualShadowMap.glslh VsmBoxMaskCells, the instance cull's same test).
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

        // A node child's or a cluster's box: in the frustum, and not hidden by the HZB when the candidate tests it
        // (CutFrame::TestHzb): last frame's through PrevViewProjection (main), this frame's through ViewProjection
        // (post). @p occludedSlot: the s_Occluded counter an occluded box adds to; @p entry: its post entry.
        bool BoxVisible( CutFrame f, GpuCullView view, vec3 center, vec3 extent, uint occludedSlot, uint entry )
        {
            vec3 boxMin;
            vec3 boxMax;
            VgWorldBox( center, extent, f.World, boxMin, boxMax );
            if ( !GpuCull_FrustumIntersects( view, boxMin, boxMax ) )
                return false;
            // A VSM level: only over a page drawn this frame (the rest of the level keeps its cached depth).
            if ( ( view.Flags & DESERT_GPU_CULL_VIEW_VSM_PAGE_MASK ) != 0u && !CoversDrawnVsmCell( view, boxMin, boxMax ) )
                return false;
            const mat4 hzbViewProjection = Params2.z != 0u ? view.ViewProjection : view.PrevViewProjection;
            if ( f.TestHzb && GpuCull_IsOccludedByHzb( ClusterCutHzb, view, hzbViewProjection, boxMin, boxMax ) )
            {
                atomicAdd( s_Occluded[occludedSlot], 1u );
                CarryOccluded( f, entry );
                return false;
            }
            return true;
        }

        void EmitCluster( CutFrame f, uint cluster )
        {
            const uint i = atomicAdd( RasterArgs.HwDraw[1], 1u );
            if ( i >= Params.y )
            {
                atomicAdd( RasterArgs.HwDraw[1], 0xFFFFFFFFu ); // net count stays the capacity
                atomicAdd( VisibleOverflow, 1u );
                atomicMin( VisibleOverflowDag, f.Dag );
                return;
            }
            Visible[Params.y - 1u - i] = VgVisibleCluster( f.Instance, cluster, f.View, 0u );
        }

        // A leaf child: its group's clusters, each by its own LodError.
        void VisitGroup( CutFrame f, GpuCullView view, uint firstCluster, uint count )
        {
            for ( uint c = 0u; c < count; ++c )
            {
                const VgCluster cluster = Clusters[firstCluster + c];
                if ( VgProjectedError( cluster.LodBounds, cluster.BoxCenter.w, f.World, f.WorldScale, f.Origin,
                                       f.LodScale, f.Orthographic ) > f.Tau )
                    continue;
                if ( !BoxVisible( f, view, cluster.BoxCenter.xyz, cluster.BoxExtent.xyz, 1u,
                                  VG_POST_CLUSTER | ( firstCluster + c ) ) )
                    continue;
                EmitCluster( f, firstCluster + c );
            }
        }

        // Child @p k of node @p node (@p n): a leaf child emits, a surviving inner child goes to the next queue,
        // else the stack.
        void VisitChild( CutFrame f, GpuCullView view, VgHierarchyNode n, uint node, uint k, uint next, bool toQueue )
        {
            const uint child = n.Child[k];
            if ( child == VG_INVALID_INDEX )
                return;
            if ( VgProjectedError( n.LodBounds[k], n.BoxExtent[k].w, f.World, f.WorldScale, f.Origin, f.LodScale,
                                   f.Orthographic ) <= f.Tau )
                return; // every cluster below is fine enough at its parent's level
            if ( !BoxVisible( f, view, n.BoxCenter[k].xyz, n.BoxExtent[k].xyz, 0u, VG_POST_CHILD | ( node << 2u ) | k ) )
                return;
            if ( ( child & VG_HIERARCHY_LEAF ) != 0u )
            {
                VisitGroup( f, view, child & ~VG_HIERARCHY_LEAF, n.ClusterCount[k] );
                return;
            }
            if ( toQueue )
            {
                const uint slot = atomicAdd( s_QueueCount[next], 1u );
                if ( slot < kQueueCapacity )
                {
                    s_Queue[next][slot] = child;
                    return;
                }
            }
            if ( g_Depth < kStackDepth )
            {
                g_Stack[g_Depth++] = child;
                return;
            }
            atomicAdd( StackOverflow, 1u );
            atomicMin( StackOverflowDag, f.Dag );
        }

        // Visits node @p node: each of its children.
        void VisitNode( CutFrame f, GpuCullView view, uint node, uint next, bool toQueue )
        {
            const VgHierarchyNode n = Nodes[node];
            for ( uint k = 0u; k < 4u; ++k )
                VisitChild( f, view, n, node, k, next, toQueue );
        }

        // The candidate's frame and view (false: not this dispatch's view, or no draw slot - counted when
        // @p countMissing).
        bool MakeFrame( VgClusterCandidate candidate, bool countMissing, out CutFrame f, out GpuCullView view )
        {
            // One dispatch per mesh pass's views (one pass table bound).
            const uint localView = candidate.View - Params3.x;
            if ( localView >= Params3.y )
                return false;
            const GpuSceneInstance instance = Instances[candidate.Instance];
            const VgClusterDag     dag      = Dags[candidate.Dag];
            view                            = Views[localView];

            // The submesh transform the view's pass placed for this primitive (lod 0 row of the DAG's submesh).
            uint slot = VG_INVALID_INDEX;
            if ( instance.Primitive < Params.z )
            {
                const GpuPrimitivePassDraws pd = PrimitiveDraws[instance.Primitive];
                if ( dag.Submesh < pd.SubmeshCount )
                    slot = pd.FirstDrawSlot + dag.Submesh;
            }
            if ( slot == VG_INVALID_INDEX )
            {
                if ( countMissing )
                {
                    atomicAdd( MissingSlot, 1u );
                    atomicMin( MissingSlotDag, candidate.Dag );
                }
                return false;
            }

            const GpuScenePrimitive primitive = Primitives[instance.Primitive];
            f.World = GpuScene_UnpackAffine( primitive.LocalToWorld ) * GpuScene_InstanceToPrimitive( instance ) *
                      SubmeshTransforms[DrawSlots[slot].SubmeshTransform];
            f.WorldScale   = VgMaxScale( f.World );
            f.Origin       = view.LodOrigin.xyz;
            f.LodScale     = ViewLods[localView].x;
            f.Tau          = ViewLods[localView].y;
            f.Orthographic = ViewLods[localView].z != 0.0;
            f.Instance     = candidate.Instance;
            f.Dag          = candidate.Dag;
            f.View         = candidate.View;
            f.TestHzb      = Params2.x != 0u && ( view.Flags & DESERT_GPU_CULL_VIEW_TEST_HZB ) != 0u &&
                        ( primitive.Flags & DESERT_GPU_PRIMITIVE_SKIP_OCCLUSION ) == 0u;
            return true;
        }

        // The workgroup's occluded node children / clusters into Status, once (after a barrier).
        void FlushOccluded()
        {
            if ( gl_LocalInvocationIndex == 0u )
            {
                if ( s_Occluded[0] != 0u )
                    atomicAdd( OccludedNodes, s_Occluded[0] );
                if ( s_Occluded[1] != 0u )
                    atomicAdd( OccludedClusters, s_Occluded[1] );
            }
        }

        // One workgroup cuts @p candidate: main = from its DAG's root, post = one carried node child's subtree.
        void CutCandidate( VgClusterCandidate candidate )
        {
            CutFrame    f;
            GpuCullView view;
            // Uniform: every thread reads the same candidate; only thread 0 counts a missing slot.
            if ( !MakeFrame( candidate, gl_LocalInvocationIndex == 0u, f, view ) )
                return;

            if ( gl_LocalInvocationIndex == 0u )
            {
                s_QueueCount[0] = 0u;
                s_QueueCount[1] = 0u;
                s_Occluded[0]   = 0u;
                s_Occluded[1]   = 0u;
            }
            barrier();
            if ( gl_LocalInvocationIndex == 0u )
            {
                if ( Params2.z == 0u )
                {
                    s_Queue[0][0]   = candidate.Node; // main: the DAG's root
                    s_QueueCount[0] = 1u;
                }
                else
                {
                    // Post: one child of a node; its subtree enters queue 0 (or this thread's stack) as in the main
                    // cut.
                    const uint node = ( candidate.Node & VG_POST_PAYLOAD ) >> 2u;
                    VisitChild( f, view, Nodes[node], node, candidate.Node & 3u, 0u, true );
                }
            }
            barrier();

            uint read = 0u;
            while ( true )
            {
                const uint count = min( s_QueueCount[read], kQueueCapacity );
                if ( count == 0u )
                    break; // uniform: every thread read the same shared count after the barrier
                const uint next = read ^ 1u;
                for ( uint e = gl_LocalInvocationIndex; e < count; e += 64u )
                    VisitNode( f, view, s_Queue[read][e], next, true );
                barrier();
                if ( gl_LocalInvocationIndex == 0u )
                    s_QueueCount[read] = 0u;
                read = next;
                barrier();
            }

            // What did not fit the queues, depth first on this thread.
            while ( g_Depth > 0u )
                VisitNode( f, view, g_Stack[--g_Depth], 0u, false );

            barrier(); // every thread reaches it: the stack loop above has no barrier inside
            FlushOccluded();
        }

        // Post: carried clusters [64 x batch, + 64) of region 1, one per thread, each re-tested alone on its own
        // candidate's frame (UE: the post pass's clusters in full batches, no one-cluster workgroup).
        void CutClusterBatch( uint batch, uint clusters )
        {
            if ( gl_LocalInvocationIndex == 0u )
            {
                s_Occluded[0] = 0u;
                s_Occluded[1] = 0u;
            }
            barrier();
            const uint e = batch * 64u + gl_LocalInvocationIndex;
            if ( e < clusters )
            {
                const VgClusterCandidate candidate = Candidates[Params.w + e];
                CutFrame                 f;
                GpuCullView              view;
                if ( MakeFrame( candidate, true, f, view ) )
                    VisitGroup( f, view, candidate.Node & VG_POST_PAYLOAD, 1u );
            }
            barrier();
            FlushOccluded();
        }

        void Cut()
        {
            const uint index = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            if ( Params2.z == 0u )
            {
                // Main: workgroup Z = the dispatch's view; its candidates are region Params3.x + Z.
                const uint region = Params3.x + gl_WorkGroupID.z;
                if ( index >= min( CandidateCounts[region], Params.w ) )
                    return; // uniform across the workgroup
                CutCandidate( Candidates[region * Params.w + index] );
                return;
            }
            // Post: the carried node children first (a workgroup each), then the carried clusters 64 per workgroup.
            const uint children = min( CandidateCounts[0], Params.w );
            if ( index < children )
                CutCandidate( Candidates[index] );
            else
                CutClusterBatch( index - children, min( CandidateCounts[1], Params.w ) );
        }

        void WriteDispatchArgs()
        {
            if ( gl_GlobalInvocationID.x != 0u )
                return;
            uint groups = 0u;
            uint layers = 1u;
            if ( Params2.z == 0u )
            {
                for ( uint v = 0u; v < Params3.y; ++v )
                    groups = max( groups, min( CandidateCounts[Params3.x + v], Params.w ) );
                layers = Params3.y;
            }
            else
                groups = min( CandidateCounts[0], Params.w ) + ( min( CandidateCounts[1], Params.w ) + 63u ) / 64u;
            DispatchX         = max( min( groups, 65535u ), 1u );
            DispatchY         = groups == 0u ? 1u : ( groups + 65534u ) / 65535u;
            DispatchZ         = groups == 0u ? 0u : layers;
            DispatchPad       = 0u;
            BinnedVgInstances = CandidateBinnedVg;
        }

        void main()
        {
            if ( Params.x == 0u )
                WriteDispatchArgs();
            else
                Cut();
        }
    }
}
