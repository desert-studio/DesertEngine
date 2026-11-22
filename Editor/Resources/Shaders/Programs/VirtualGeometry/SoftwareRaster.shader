// DesertAsset {"Kind":"Shader","Guid":"40f365ec79e74a93b8ce1cb9c2d66205","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SoftwareRaster"
{
    // "VirtualGeometry: Software Raster" (Engine/Graphic/VirtualGeometry/VirtualGeometryRenderer.cpp
    // AddSoftwareRaster; UE 5 Shaders/Private/Nanite/NaniteRasterizer.usf MicropolyRasterize + RasterizeTri): the
    // cluster cut's SW clusters (VisibleClusters[0, RasterArgs.SwCount), ClusterCut.shader RastersInSoftware) drawn
    // into the view's visibility buffer (Common/VisibilityBuffer.glslh's layout: a 16-byte header with the extent,
    // then one uint64 word per pixel) by 64-bit atomicMax of (reversed-Z depth bits << 32 | payload), payload =
    // (Params.x + the VisibleClusters row) << kVisTriangleBits | triangle - Graphic::VirtualGeometry::PackVisibility.
    //
    // One workgroup per SW cluster (RasterArgs.SwDispatch, at most VG_SW_RASTER_MAX_GROUPS; the workgroups stride
    // over the rest). Step 1: thread v < VertexCount transforms cluster vertex v once into shared memory (UE: the
    // lanes transform the cluster's vertices, the triangles read them). Step 2: thread t < TriangleCount
    // rasterises triangle t over its pixel box.
    //
    // THE HW RASTER'S RULES, so a cluster that changes raster between frames does not move a pixel:
    //  - the same transform (the pass's lod-0 draw slot of the DAG submesh, as ClusterCut and the `.VirtualGeometry`
    //    cells take it) through the same JITTERED view-projection (PushConstants.ViewProjection);
    //  - the viewport VulkanRendererAPI::SetViewportAndScissor sets (negative height: framebuffer row y is NDC
    //    1 - 2 y / H), pixel centres at +0.5, vertices snapped to 1/256 pixel (subPixelPrecisionBits = 8, the
    //    common device value) and the edge functions evaluated exactly in int64;
    //  - front face = counter-clockwise in framebuffer space (VulkanPipeline.cpp VK_FRONT_FACE_COUNTER_CLOCKWISE):
    //    the cut sends only slots culling Back or None (DESERT_GPU_DRAW_SLOT_SOFTWARE_RASTER); a back face is
    //    dropped unless the slot is DESERT_GPU_DRAW_SLOT_TWO_SIDED, which flips it to front winding;
    //  - depth = NDC z interpolated linearly in screen space (z / w is affine there), reversed-Z (nearer = larger).
    // A pixel on a shared edge is taken by BOTH triangles (an inclusive edge test, not the top-left rule): the
    // atomicMax keeps one of two equal-depth words, so no pixel of a closed surface is ever left out (a top-left
    // mismatch would punch pinholes; a double take costs one atomic).
    // The cut sends only clusters wholly in front of the near plane (every w > 0, no clipping here) and drawn
    // without wind (no world position offset).
    Compute
    {
        LocalSize(128, 1, 1);

        #extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
        #extension GL_EXT_shader_atomic_int64 : require

        #include <Common/GpuScene.glslh>
        #include <Common/VirtualGeometry.glslh>

        ReadBuffer(0) SoftwareRasterPrimitives
        {
            GpuScenePrimitive Primitives[];
        };
        ReadBuffer(1) SoftwareRasterInstances
        {
            GpuSceneInstance Instances[];
        };
        ReadBuffer(2) SoftwareRasterPrimitiveDraws
        {
            GpuPrimitivePassDraws PrimitiveDraws[];
        };
        ReadBuffer(3) SoftwareRasterDrawSlots
        {
            GpuDrawSlot DrawSlots[];
        };
        ReadBuffer(4) SoftwareRasterSubmeshTransforms
        {
            mat4 SubmeshTransforms[];
        };
        ReadBuffer(5) SoftwareRasterDags
        {
            VgClusterDag Dags[];
        };
        ReadBuffer(6) SoftwareRasterClusters
        {
            VgCluster Clusters[];
        };
        ReadBuffer(7) SoftwareRasterVisibleClusters
        {
            VgVisibleCluster Visible[];
        };
        ReadBuffer(8) SoftwareRasterArgs
        {
            VgRasterArgs RasterArgs;
        };
        Buffer(9) SoftwareRasterVisibility
        {
            uvec4    VisibilityHeader; // x = width, y = height (VisibilityClear.shader)
            uint64_t VisibilityWords[];
        };

        PushConstant PushConstants
        {
            mat4  ViewProjection; // the main view's jittered view-projection (the HW raster's)
            uvec4 Params;         // x = the cut's first visibility row (rowBase), y = visible capacity
        };

        #define kVisTriangleBits 7u
        #define kSubpixelBits    8
        #define kSubpixelHalf    128
        #define kSubpixelScale   256.0

        // Per cluster vertex: the snapped framebuffer position (1/256 px) and the reversed-Z NDC depth.
        shared ivec2 s_Position[128];
        shared float s_Depth[128];

        // Twice the signed area of (p0, p1, p2) in framebuffer space (y down): < 0 = counter-clockwise = front.
        int64_t TwiceArea( ivec2 p0, ivec2 p1, ivec2 p2 )
        {
            return int64_t( p1.x - p0.x ) * int64_t( p2.y - p0.y ) - int64_t( p2.x - p0.x ) * int64_t( p1.y - p0.y );
        }

        // Edge function of a -> b at sample s: <= 0 on the inner side of a front-wound triangle's edge.
        int64_t Edge( ivec2 a, ivec2 b, ivec2 s )
        {
            return int64_t( b.x - a.x ) * int64_t( s.y - a.y ) - int64_t( b.y - a.y ) * int64_t( s.x - a.x );
        }

        void RasterTriangle( ivec2 p0, ivec2 p1, ivec2 p2, float z0, float z1, float z2, uint payload, bool twoSided )
        {
            int64_t area = TwiceArea( p0, p1, p2 );
            if ( area == 0 )
                return;
            if ( area > 0 )
            {
                if ( !twoSided )
                    return; // a back face, culled as the cell's CullMode::Back culls it
                const ivec2 tp = p1;
                p1             = p2;
                p2             = tp;
                const float tz = z1;
                z1             = z2;
                z2             = tz;
                area           = -area;
            }
            // The pixels whose centre (x + 0.5, y + 0.5) the snapped box covers, clipped to the buffer.
            const ivec2 boxMin = min( min( p0, p1 ), p2 );
            const ivec2 boxMax = max( max( p0, p1 ), p2 );
            const ivec2 lo = max( ( boxMin - ivec2( kSubpixelHalf ) + ivec2( 255 ) ) >> kSubpixelBits, ivec2( 0 ) );
            const ivec2 hi = min( ( boxMax - ivec2( kSubpixelHalf ) ) >> kSubpixelBits,
                                  ivec2( VisibilityHeader.xy ) - ivec2( 1 ) );
            if ( any( greaterThan( lo, hi ) ) )
                return;
            const float total = float( area ); // e0 + e1 + e2 at any sample (< 0)
            for ( int y = lo.y; y <= hi.y; ++y )
            {
                for ( int x = lo.x; x <= hi.x; ++x )
                {
                    const ivec2   s  = ivec2( ( x << kSubpixelBits ) + kSubpixelHalf, ( y << kSubpixelBits ) + kSubpixelHalf );
                    const int64_t e0 = Edge( p1, p2, s ); // the weight of p0
                    const int64_t e1 = Edge( p2, p0, s ); // of p1
                    const int64_t e2 = Edge( p0, p1, s ); // of p2
                    // Inside: every edge function <= 0 (inclusive, see the header).
                    if ( e0 > 0 || e1 > 0 || e2 > 0 )
                        continue;
                    const float depth = ( float( e0 ) * z0 + float( e1 ) * z1 + float( e2 ) * z2 ) / total;
                    if ( !( depth > 0.0 ) || depth > 1.0 )
                        continue;
                    const uint     index = uint( y ) * VisibilityHeader.x + uint( x );
                    const uint64_t word  = ( uint64_t( floatBitsToUint( depth ) ) << 32 ) | uint64_t( payload );
                    // The word only grows during the raster: a load already holding it or more proves the atomic
                    // would change nothing (Common/VisibilityBuffer.glslh DesertWriteVisibility).
                    if ( VisibilityWords[index] < word )
                        atomicMax( VisibilityWords[index], word );
                }
            }
        }

        void RasterCluster( uint row )
        {
            const VgVisibleCluster  visible   = Visible[row];
            const VgCluster         cluster   = Clusters[visible.Cluster];
            const VgClusterDag      dag       = Dags[cluster.Dag];
            const GpuSceneInstance  instance  = Instances[visible.Instance];
            const GpuScenePrimitive primitive = Primitives[instance.Primitive];
            // The pass's lod-0 slot of the DAG's submesh: the one the cut took the transform and the SW flags from.
            const GpuDrawSlot drawSlot = DrawSlots[PrimitiveDraws[instance.Primitive].FirstDrawSlot + dag.Submesh];
            const mat4 world = GpuScene_UnpackAffine( primitive.LocalToWorld ) * GpuScene_InstanceToPrimitive( instance ) *
                               SubmeshTransforms[drawSlot.SubmeshTransform];
            const mat4 toClip = ViewProjection * world;

            const uint t = gl_LocalInvocationIndex;
            if ( t < VgClusterVertexCount( cluster ) )
            {
                const uint ref    = VgWords( dag.VertexRefAddress ).Words[cluster.FirstVertexRef + t];
                const vec3 local  = VgFetchFloat3( VgFloats( dag.VertexAddress ), ref * ( dag.VertexStride / 4u ) );
                const vec4 clip   = toClip * vec4( local, 1.0 );
                const vec2 ndc    = clip.xy / clip.w;
                const vec2 extent = vec2( VisibilityHeader.xy );
                // Framebuffer position (y down: row y is NDC 1 - 2 y / H), snapped to 1/256 pixel.
                const vec2 pixel = vec2( ( ndc.x * 0.5 + 0.5 ) * extent.x, ( 0.5 - ndc.y * 0.5 ) * extent.y );
                s_Position[t]    = ivec2( round( pixel * kSubpixelScale ) );
                s_Depth[t]       = clip.z / clip.w;
            }
            barrier();
            if ( t < VgClusterTriangleCount( cluster ) )
            {
                const uint word    = VgWords( dag.TriangleAddress ).Words[cluster.FirstTriangle + t];
                const uint i0      = word & 0xFFu;
                const uint i1      = ( word >> 8u ) & 0xFFu;
                const uint i2      = ( word >> 16u ) & 0xFFu;
                const uint payload = ( ( Params.x + row ) << kVisTriangleBits ) | t;
                RasterTriangle( s_Position[i0], s_Position[i1], s_Position[i2], s_Depth[i0], s_Depth[i1], s_Depth[i2],
                                payload, ( drawSlot.Flags & DESERT_GPU_DRAW_SLOT_TWO_SIDED ) != 0u );
            }
            barrier(); // the next cluster of a striding workgroup overwrites the shared vertices
        }

        void main()
        {
            const uint count = min( RasterArgs.SwCount, Params.y );
            // Uniform per workgroup: the bound and the row are the same for every lane, so the whole workgroup
            // reaches the barriers in RasterCluster.
            for ( uint row = gl_WorkGroupID.x; row < count; row += gl_NumWorkGroups.x )
                RasterCluster( row );
        }
    }
}
