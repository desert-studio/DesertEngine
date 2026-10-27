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

        Uniform sampler PointClampSampler;
        Uniform(15) texture2D GpuCullHzbTexture; // the furthest HZB of the dispatch's HZB view (last frame's in step 1)
        #define GpuCullHzb sampler2D(GpuCullHzbTexture, PointClampSampler)

        PushConstant PushConstants
        {
            uvec4 Params;  // x = step, y = instance rows, z = views of this dispatch, w = first view in Views[]
            uvec4 Params2; // x = bins of the pass, y = visible capacity per view, z = primitive rows of the pass
                           // table, w = 1 when a HZB is bound
        };

        uint ThreadIndex()
        {
            return ( gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x ) * 64u + gl_LocalInvocationIndex;
        }

        // Core::Frustum::Intersects: the corner farthest along each plane's normal; a zero plane accepts.
        bool FrustumIntersects( GpuCullView view, vec3 boxMin, vec3 boxMax )
        {
            for ( uint p = 0u; p < 6u; ++p )
            {
                const vec4 plane    = view.FrustumPlanes[p];
                const vec3 positive = vec3( plane.x >= 0.0 ? boxMax.x : boxMin.x, plane.y >= 0.0 ? boxMax.y : boxMin.y,
                                            plane.z >= 0.0 ? boxMax.z : boxMin.z );
                if ( dot( positive, plane.xyz ) + plane.w < 0.0 )
                    return false;
            }
            return true;
        }

        // "VSM: Cull Casters"' page test (Programs/Shadows/VirtualShadowMapCullCasters.shader), same arithmetic.
        bool CoversDrawnVsmCell( GpuCullView view, vec3 boxMin, vec3 boxMax )
        {
            vec2  first   = vec2( 1.0e30 );
            vec2  last    = vec2( -1.0e30 );
            float nearest = 1.0e30;
            for ( uint c = 0u; c < 8u; ++c )
            {
                const vec3 corner =
                     mix( boxMin, boxMax, vec3( float( c & 1u ), float( ( c >> 1u ) & 1u ), float( ( c >> 2u ) & 1u ) ) );
                const vec4 clip = view.ViewProjection * vec4( corner, 1.0 );
                const vec2 page = ( clip.xy * 0.5 + 0.5 ) * float( VSM_LEVEL_PAGES );
                first           = min( first, page );
                last            = max( last, page );
                nearest         = min( nearest, clip.z );
            }
            if ( nearest > 1.0 )
                return false;
            const ivec2 lowPage  = max( ivec2( floor( first ) ), ivec2( 0 ) );
            const ivec2 highPage = min( ivec2( floor( last ) ), ivec2( int( VSM_LEVEL_PAGES ) - 1 ) );
            if ( any( greaterThan( lowPage, highPage ) ) )
                return false;
            const ivec2 lowCell  = lowPage / int( VSM_MASK_CELL_PAGES );
            const ivec2 highCell = highPage / int( VSM_MASK_CELL_PAGES );
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
                const float half = 0.5 * LodScreenSizes[primitive.FirstLodScreenSize + uint( level )];
                if ( half * half > screenRadiusSq )
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

        // FURTHEST-HZB occlusion (UE HZBOcclusion.usf IsVisibleHZB): the box's screen rect through @p viewProjection,
        // the mip at which it spans at most 2 x 2 texels, the furthest depth of those texels against the box's
        // nearest depth. Reversed-Z (1 = near): occluded when the box's nearest is below every texel's furthest.
        // A box crossing the near plane, or leaving the screen, is not occluded.
        bool IsOccludedByHzb( GpuCullView view, mat4 viewProjection, vec3 boxMin, vec3 boxMax )
        {
            vec2  rectMin = vec2( 1.0e30 );
            vec2  rectMax = vec2( -1.0e30 );
            float nearest = 0.0;
            for ( uint c = 0u; c < 8u; ++c )
            {
                const vec3 corner =
                     mix( boxMin, boxMax, vec3( float( c & 1u ), float( ( c >> 1u ) & 1u ), float( ( c >> 2u ) & 1u ) ) );
                const vec4 clip = viewProjection * vec4( corner, 1.0 );
                if ( clip.w <= 0.0 )
                    return false;
                const vec3 ndc = clip.xyz / clip.w;
                // Vulkan clip space: +y is down the screen, so uv.y = ndc.y * 0.5 + 0.5 is the texel row.
                const vec2 uv = ndc.xy * 0.5 + 0.5;
                rectMin       = min( rectMin, uv );
                rectMax       = max( rectMax, uv );
                nearest       = max( nearest, ndc.z );
            }
            if ( nearest >= 1.0 || any( lessThan( rectMin, vec2( 0.0 ) ) ) || any( greaterThan( rectMax, vec2( 1.0 ) ) ) )
                return false; // touches the near plane or leaves the HZB: last frame's depth does not cover it
            const vec2  size    = ( rectMax - rectMin ) * vec2( view.HzbExtent.xy );
            const uint  mips    = max( view.HzbExtent.z, 1u );
            const float level   = ceil( log2( max( max( size.x, size.y ), 1.0 ) ) );
            const int   mip     = int( clamp( level, 0.0, float( mips - 1u ) ) );
            const ivec2 mipSize = max( ivec2( view.HzbExtent.xy ) >> mip, ivec2( 1 ) );
            const ivec2 t0      = clamp( ivec2( floor( rectMin * vec2( mipSize ) ) ), ivec2( 0 ), mipSize - 1 );
            const ivec2 t1      = clamp( ivec2( floor( rectMax * vec2( mipSize ) ) ), ivec2( 0 ), mipSize - 1 );
            float       furthest = 1.0;
            for ( int y = t0.y; y <= t1.y; ++y )
                for ( int x = t0.x; x <= t1.x; ++x )
                    furthest = min( furthest, texelFetch( GpuCullHzb, ivec2( x, y ), mip ).r );
            return nearest < furthest;
        }

        // Counts the instance into each submesh bin of its LOD in the pass and writes its visible entries.
        void EmitToBins( uint localView, uint instanceRow, uint primitiveRow, uint lod )
        {
            if ( primitiveRow >= Params2.z )
                return;
            const GpuPrimitivePassDraws pd = PrimitiveDraws[primitiveRow];
            if ( pd.SubmeshCount == 0u || pd.LodCount == 0u )
                return;
            const uint level = min( lod, pd.LodCount - 1u );
            for ( uint s = 0u; s < pd.SubmeshCount; ++s )
            {
                const uint slot = pd.FirstDrawSlot + level * pd.SubmeshCount + s;
                const uint bin  = DrawSlots[slot].Bin;
                const uint k    = atomicAdd( Draws[localView * Params2.x + bin].InstanceCount, 1u );
                const uint entry = localView * Params2.y + Bins[bin].FirstVisible + k;
                Visible[entry]            = GpuVisibleInstance( instanceRow, slot );
                VisibleInstanceIds[entry] = instanceRow;
            }
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
                bool inside = FrustumIntersects( view, boxMin, boxMax );
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

            if ( !empty && Params2.w != 0u && ( view.Flags & DESERT_GPU_CULL_VIEW_TEST_HZB ) != 0u &&
                 ( primitive.Flags & DESERT_GPU_PRIMITIVE_SKIP_OCCLUSION ) == 0u &&
                 IsOccludedByHzb( view, view.PrevViewProjection, boxMin, boxMax ) )
            {
                const uint slot = atomicAdd( OccludedCount, 1u );
                if ( slot < OccludedCapacity )
                    Occluded[slot] = GpuOccludedInstance( instanceRow, view.ViewIndex );
                Visibility[cell] = word | DESERT_GPU_CULL_VISIBILITY_OCCLUDED;
                return;
            }
            Visibility[cell] = word;
            EmitToBins( localView, instanceRow, instance.Primitive, lod );
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
            if ( Params2.w != 0u && IsOccludedByHzb( view, view.ViewProjection, boxMin, boxMax ) )
                return;
            const uint cell = entry.View * Params.y + entry.Instance;
            const uint lod  = Visibility[cell] & DESERT_GPU_CULL_VISIBILITY_LOD_MASK;
            atomicOr( Visibility[cell], DESERT_GPU_CULL_VISIBILITY_POST_VISIBLE );
            EmitToBins( localView, entry.Instance, instance.Primitive, lod );
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
