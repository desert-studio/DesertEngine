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
    //
    // OCCLUSION: the instance cull appends VG candidates before its HZB test, so the cut owns VG occlusion. A node
    // child and a cluster past the frustum test are tested against the PREVIOUS frame's furthest HZB through
    // PrevViewProjection with the instance cull's own test (Common/GpuCullTests.glslh GpuCull_IsOccludedByHzb, one
    // source), when a HZB is bound (Params2.x), the view tests it (DESERT_GPU_CULL_VIEW_TEST_HZB) and the primitive
    // does not skip occlusion. The occluded are counted in Status (OccludedNodes / OccludedClusters) and dropped:
    // no carry into a post phase yet (the lit image still draws VG instances through the bins, so the HZB those
    // bins build stays complete).
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
            uint StatusPad;
        };
        Buffer(15) ClusterCutDispatchArgs
        {
            uint DispatchX;
            uint DispatchY;
            uint DispatchZ;
            uint DispatchPad;
        };

        Uniform sampler PointClampSampler;
        Uniform(16) texture2D ClusterCutHzbTexture; // the previous frame's furthest HZB of the views (neutral: none)
        #define ClusterCutHzb sampler2D(ClusterCutHzbTexture, PointClampSampler)

        PushConstant PushConstants
        {
            uvec4 Params;  // x = step, y = visible capacity, z = primitive rows of the pass table, w = candidate capacity
            uvec4 Params2; // x = 1 when a HZB is bound, yzw = 0
        };

        #define kQueueCapacity 512u
        #define kStackDepth    48u

        shared uint s_Queue[2][kQueueCapacity];
        shared uint s_QueueCount[2];
        shared uint s_Occluded[2]; // the workgroup's occluded node children / clusters, added to Status once
        // The thread's own depth-first stack (per invocation, not shared).
        uint g_Stack[kStackDepth];
        uint g_Depth = 0u;

        // A node child's or a cluster's box: in the frustum, and not hidden by last frame's furthest HZB when the
        // candidate tests it (CutFrame::TestHzb). @p occludedSlot: the s_Occluded counter an occluded box adds to.
        bool BoxVisible( GpuCullView view, vec3 center, vec3 extent, mat4 world, bool testHzb, uint occludedSlot )
        {
            vec3 boxMin;
            vec3 boxMax;
            VgWorldBox( center, extent, world, boxMin, boxMax );
            if ( !GpuCull_FrustumIntersects( view, boxMin, boxMax ) )
                return false;
            if ( testHzb && GpuCull_IsOccludedByHzb( ClusterCutHzb, view, view.PrevViewProjection, boxMin, boxMax ) )
            {
                atomicAdd( s_Occluded[occludedSlot], 1u );
                return false;
            }
            return true;
        }

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
                if ( !BoxVisible( view, cluster.BoxCenter.xyz, cluster.BoxExtent.xyz, f.World, f.TestHzb, 1u ) )
                    continue;
                EmitCluster( f, firstCluster + c );
            }
        }

        // Visits node @p node: leaf children emit, surviving inner children go to the next queue, else the stack.
        void VisitNode( CutFrame f, GpuCullView view, uint node, uint next, bool toQueue )
        {
            const VgHierarchyNode n = Nodes[node];
            for ( uint k = 0u; k < 4u; ++k )
            {
                const uint child = n.Child[k];
                if ( child == VG_INVALID_INDEX )
                    continue;
                if ( VgProjectedError( n.LodBounds[k], n.BoxExtent[k].w, f.World, f.WorldScale, f.Origin,
                                       f.LodScale ) <= f.Tau )
                    continue; // every cluster below is fine enough at its parent's level
                if ( !BoxVisible( view, n.BoxCenter[k].xyz, n.BoxExtent[k].xyz, f.World, f.TestHzb, 0u ) )
                    continue;
                if ( ( child & VG_HIERARCHY_LEAF ) != 0u )
                {
                    VisitGroup( f, view, child & ~VG_HIERARCHY_LEAF, n.ClusterCount[k] );
                    continue;
                }
                if ( toQueue )
                {
                    const uint slot = atomicAdd( s_QueueCount[next], 1u );
                    if ( slot < kQueueCapacity )
                    {
                        s_Queue[next][slot] = child;
                        continue;
                    }
                }
                if ( g_Depth < kStackDepth )
                {
                    g_Stack[g_Depth++] = child;
                    continue;
                }
                atomicAdd( StackOverflow, 1u );
                atomicMin( StackOverflowDag, f.Dag );
            }
        }

        void Cut()
        {
            const uint candidateIndex = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            if ( candidateIndex >= min( CandidateCount, Params.w ) )
                return; // uniform across the workgroup
            const VgClusterCandidate candidate = Candidates[candidateIndex];
            const GpuSceneInstance   instance  = Instances[candidate.Instance];
            const VgClusterDag       dag       = Dags[candidate.Dag];
            const GpuCullView        view      = Views[candidate.View];

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
            f.LodScale   = ViewLods[candidate.View].x;
            f.Tau        = ViewLods[candidate.View].y;
            f.Instance   = candidate.Instance;
            f.Dag        = candidate.Dag;
            f.View       = candidate.View;
            f.TestHzb    = Params2.x != 0u && ( view.Flags & DESERT_GPU_CULL_VIEW_TEST_HZB ) != 0u &&
                        ( primitive.Flags & DESERT_GPU_PRIMITIVE_SKIP_OCCLUSION ) == 0u;

            if ( gl_LocalInvocationIndex == 0u )
            {
                s_Queue[0][0]   = candidate.Node;
                s_QueueCount[0] = 1u;
                s_QueueCount[1] = 0u;
                s_Occluded[0]   = 0u;
                s_Occluded[1]   = 0u;
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
