// DesertAsset {"Kind":"Shader","Guid":"260719df60b248f1be2a86aab971ea0e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualGeometryClusters"
{
    // The vg_clusters view (DebugViewState::VirtualGeometry = Clusters; UE r.Nanite.Visualize Clusters): every
    // level-0 cluster of each drawn virtual-geometry submesh, re-drawn from the VirtualMeshTable rows - not from
    // the mesh's index buffer - in a flat colour hashed from its cluster row, over the lit scene and depth-tested
    // against it (CloserOrEqual, plus a relative nudge toward the camera so the same triangles win over the
    // scene's own draw). One vertexless instanced draw per submesh DAG: instance = cluster, vertex = triangle * 3
    // + corner; a cluster of a coarser level, or a triangle past the cluster's count, collapses.
    State
    {
        Topology Triangles
        Cull None
    }

    Vertex
    {
        #include <Common/VirtualGeometry.glslh>
        ReadBuffer(0) VgClusters
        {
            VgCluster vgClusters[];
        };
        ReadBuffer(1) VgDags
        {
            VgClusterDag vgDags[];
        };
        PushConstant ClusterViewPush
        {
            mat4 WorldViewProjection; // jittered view-projection * primitive world * submesh transform
            uint FirstCluster;
            uint ClusterCount;
        };
        layout(location = 0) out flat uint v_Cluster;

        void main()
        {
            const uint      row      = FirstCluster + uint( gl_InstanceIndex );
            const VgCluster cluster  = vgClusters[row];
            const uint      triangle = uint( gl_VertexIndex ) / 3u;
            v_Cluster                = row;
            if ( VgClusterLevel( cluster ) != 0u || triangle >= VgClusterTriangleCount( cluster ) )
            {
                gl_Position = vec4( 0.0, 0.0, -2.0, 1.0 );
                return;
            }
            const VgClusterDag dag      = vgDags[cluster.Dag];
            const vec3         position = VgClusterCornerPosition( dag, cluster, triangle, uint( gl_VertexIndex ) % 3u );
            gl_Position                 = WorldViewProjection * vec4( position, 1.0 );
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
