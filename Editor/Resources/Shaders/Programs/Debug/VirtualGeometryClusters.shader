// DesertAsset {"Kind":"Shader","Guid":"260719df60b248f1be2a86aab971ea0e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualGeometryClusters"
{
    // The vg_clusters view (DebugViewState::VirtualGeometry = Clusters; UE r.Nanite.Visualize Clusters): the
    // clusters this frame's cluster cut chose ("VirtualGeometry: Cluster Cut", ClusterCut.shader), re-drawn from
    // the VirtualMeshTable rows - not from the mesh's index buffer - in a flat colour hashed from the cluster row,
    // over the lit scene and depth-tested against it (CloserOrEqual, plus a relative nudge toward the camera so the
    // same triangles win over the scene's own draw). ONE indirect draw on the cut's RasterArgs.HwDraw: instance i
    // = the HW cluster VisibleClusters[VisibleCapacity - 1 - i] (HW from the back, the UE layout), vertex =
    // triangle * 3 + corner; a triangle past the cluster's count collapses. The world is the cut's: primitive x
    // instance x the view pass's lod-0 submesh transform of the DAG's submesh.
    State
    {
        Topology Triangles
        Cull None
    }

    Vertex
    {
        #include <Common/GpuScene.glslh>
        #include <Common/VirtualGeometry.glslh>
        ReadBuffer(0) VgClusters
        {
            VgCluster vgClusters[];
        };
        ReadBuffer(1) VgDags
        {
            VgClusterDag vgDags[];
        };
        ReadBuffer(2) VgPrimitives
        {
            GpuScenePrimitive vgPrimitives[];
        };
        ReadBuffer(3) VgInstances
        {
            GpuSceneInstance vgInstances[];
        };
        ReadBuffer(4) VgPassPrimitiveDraws
        {
            GpuPrimitivePassDraws vgPrimitiveDraws[];
        };
        ReadBuffer(5) VgPassDrawSlots
        {
            GpuDrawSlot vgDrawSlots[];
        };
        ReadBuffer(6) VgSubmeshTransforms
        {
            mat4 vgSubmeshTransforms[];
        };
        ReadBuffer(7) VgVisibleClusters
        {
            VgVisibleCluster vgVisible[];
        };
        PushConstant ClusterViewPush
        {
            mat4 ViewProjection; // the view's jittered view-projection
            uint VisibleCapacity;
            uint Pad0;
            uint Pad1;
            uint Pad2;
        };
        layout(location = 0) out flat uint v_Cluster;

        void main()
        {
            const VgVisibleCluster visible  = vgVisible[VisibleCapacity - 1u - uint( gl_InstanceIndex )];
            const VgCluster        cluster  = vgClusters[visible.Cluster];
            const uint             triangle = uint( gl_VertexIndex ) / 3u;
            v_Cluster                       = visible.Cluster;
            if ( triangle >= VgClusterTriangleCount( cluster ) )
            {
                gl_Position = vec4( 0.0, 0.0, -2.0, 1.0 );
                return;
            }
            const VgClusterDag          dag      = vgDags[cluster.Dag];
            const GpuSceneInstance      instance = vgInstances[visible.Instance];
            // The cut emitted this cluster only with a draw slot for the DAG's submesh (ClusterCut.shader Cut).
            const GpuPrimitivePassDraws pd       = vgPrimitiveDraws[instance.Primitive];
            const mat4 world = GpuScene_UnpackAffine( vgPrimitives[instance.Primitive].LocalToWorld ) *
                               GpuScene_InstanceToPrimitive( instance ) *
                               vgSubmeshTransforms[vgDrawSlots[pd.FirstDrawSlot + dag.Submesh].SubmeshTransform];
            const vec3 position = VgClusterCornerPosition( dag, cluster, triangle, uint( gl_VertexIndex ) % 3u );
            gl_Position         = ViewProjection * world * vec4( position, 1.0 );
            // Reversed-Z: nearer is larger. A relative nudge keeps the overlay in front of the scene's draw of the
            // same triangles (another matrix order) at every distance.
            gl_Position.z *= 1.0 + 2e-5;
        }
    }

    Fragment
    {
        layout(location = 0) in flat uint v_Cluster;
        Out(0) vec4 o_Color;

        // PCG hash: one stable colour per cluster row.
        uint Hash( uint v )
        {
            uint state = v * 747796405u + 2891336453u;
            uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
            return ( word >> 22u ) ^ word;
        }

        void main()
        {
            const uint h = Hash( v_Cluster );
            o_Color      = vec4( vec3( h & 0xFFu, ( h >> 8u ) & 0xFFu, ( h >> 16u ) & 0xFFu ) / 255.0, 1.0 );
        }
    }
}
