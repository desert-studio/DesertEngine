// DesertAsset {"Kind":"Shader","Guid":"160b36d01f8843f389a49ec2291e7d32","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ClusterCut"
{
    // "VirtualGeometry: Cluster Cut" (Engine/Graphic/VirtualGeometry/VirtualGeometryRenderer.cpp AddClusterCull;
    // UE 5.8 Shaders/Private/Nanite/NaniteClusterCulling.usf NodeAndClusterCull + NaniteHierarchyTraversal.ush):
    // which clusters of each VG candidate (instance x submesh DAG x view, appended by InstanceCulling) are drawn.
    //
    // One program, two steps (Params.x):
    //  0 ARGS  one thread: the candidate count -> step 1's VkDispatchIndirectCommand (one workgroup per candidate).
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
    //          one shadow cascade): the candidates of other views return at once.
    //
    // OCCLUSION: the instance cull appends VG candidates before its HZB test, so the cut owns VG occlusion. A node
    // child and a cluster past the frustum test are tested against the PREVIOUS frame's furthest HZB through
    // PrevViewProjection with the instance cull's own test (Common/GpuCullTests.glslh GpuCull_IsOccludedByHzb, one
    // source), when a HZB is bound (Params2.x), the view tests it (DESERT_GPU_CULL_VIEW_TEST_HZB) and the primitive
    // does not skip occlusion. The occluded are counted in Status (OccludedNodes / OccludedClusters) and, in the
    // MAIN phase with a carry (Params2.y = its capacity), appended to OccludedCandidates for the POST phase (UE
    // NaniteClusterCulling.usf: occluded nodes / clusters into the post pass's queues): an occluded node child as
    // VG_POST_CHILD | (node << 2 | k), an occluded cluster as VG_POST_CLUSTER | cluster, in the candidate's Node.
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
            vec4 ViewLods[]; // per view: x = lodScale (0.5 x height x LodProjection[1][1]), y = tau (pixels per edge)
        };
        ReadBuffer(11) ClusterCutCandidateCount
        {
            uint CandidateCount;    // the producer's append count (may pass Params.w: clamped)
            uint CandidateBinnedVg; // InstanceCull's VgBinnedInstances (0 = no VG instance in the view's bins)
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
            VgClusterCandidate OccludedCandidates[]; // main phase: the post phase's candidates (Node encoded)
        };
        Buffer(18) ClusterCutOccludedCount
        {
            uint OccludedCount;    // the post phase's CandidateCount (same block layout)
            uint OccludedBinnedVg; // stays 0
        };

        Uniform sampler PointClampSampler;
        Uniform(16) texture2D ClusterCutHzbTexture; // the previous frame's furthest HZB of the views (neutral: none)
        #define ClusterCutHzb sampler2D(ClusterCutHzbTexture, PointClampSampler)

        PushConstant PushConstants
        {
            uvec4 Params;  // x = step, y = visible capacity, z = primitive rows of the pass table, w = candidate capacity
            uvec4 Params2; // x = 1 when a HZB is bound, y = the occluded carry's capacity (main; 0 = none),
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
            const uint i = atomicAdd( OccludedCount, 1u );
            if ( i >= Params2.y )
            {
                atomicAdd( OccludedCount, 0xFFFFFFFFu ); // net count stays the capacity
                atomicAdd( OccludedOverflow, 1u );
                return;
            }
            OccludedCandidates[i] = VgClusterCandidate( f.Instance, f.Dag, entry, f.View );
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
                                       f.LodScale ) > f.Tau )
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
            if ( VgProjectedError( n.LodBounds[k], n.BoxExtent[k].w, f.World, f.WorldScale, f.Origin, f.LodScale ) <=
                 f.Tau )
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

        void Cut()
        {
            const uint candidateIndex = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            if ( candidateIndex >= min( CandidateCount, Params.w ) )
                return; // uniform across the workgroup
            const VgClusterCandidate candidate = Candidates[candidateIndex];
            // One dispatch per mesh pass's views (one pass table bound): another dispatch's candidate is skipped.
            const uint localView = candidate.View - Params3.x;
            if ( localView >= Params3.y )
                return; // uniform
            const GpuSceneInstance   instance  = Instances[candidate.Instance];
            const VgClusterDag       dag       = Dags[candidate.Dag];
            const GpuCullView        view      = Views[localView];

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
                if ( gl_LocalInvocationIndex == 0u )
                {
                    atomicAdd( MissingSlot, 1u );
                    atomicMin( MissingSlotDag, candidate.Dag );
                }
                return; // uniform
            }

            const GpuScenePrimitive primitive = Primitives[instance.Primitive];
            CutFrame                f;
            f.World = GpuScene_UnpackAffine( primitive.LocalToWorld ) *
                      GpuScene_InstanceToPrimitive( instance ) * SubmeshTransforms[DrawSlots[slot].SubmeshTransform];
            f.WorldScale = VgMaxScale( f.World );
            f.Origin     = view.LodOrigin.xyz;
            f.LodScale   = ViewLods[localView].x;
            f.Tau        = ViewLods[localView].y;
            f.Instance   = candidate.Instance;
            f.Dag        = candidate.Dag;
            f.View       = candidate.View;
            f.TestHzb    = Params2.x != 0u && ( view.Flags & DESERT_GPU_CULL_VIEW_TEST_HZB ) != 0u &&
                        ( primitive.Flags & DESERT_GPU_PRIMITIVE_SKIP_OCCLUSION ) == 0u;

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
                else if ( ( candidate.Node & VG_POST_CLUSTER ) != 0u )
                    VisitGroup( f, view, candidate.Node & VG_POST_PAYLOAD, 1u ); // post: one cluster
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
            if ( gl_LocalInvocationIndex == 0u )
            {
                if ( s_Occluded[0] != 0u )
                    atomicAdd( OccludedNodes, s_Occluded[0] );
                if ( s_Occluded[1] != 0u )
                    atomicAdd( OccludedClusters, s_Occluded[1] );
            }
        }

        void WriteDispatchArgs()
        {
            if ( gl_GlobalInvocationID.x != 0u )
                return;
            const uint groups = min( CandidateCount, Params.w );
            DispatchX         = max( min( groups, 65535u ), 1u );
            DispatchY         = groups == 0u ? 1u : ( groups + 65534u ) / 65535u;
            DispatchZ         = groups == 0u ? 0u : 1u;
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
