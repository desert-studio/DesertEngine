// DesertAsset {"Kind":"Shader","Guid":"dbedf932ae4a42829e8323fc194b1f4c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LandscapeVirtualTexturePage"
{
    Compute
    {
        // "Landscape: Virtual Texture Pages" (Graphic/Systems/Scene/Terrain/LandscapeVirtualTexture.cpp; UE Runtime
        // Virtual Texture page render of the landscape material). One workgroup layer per job: the 136 x 136 texels
        // of one page (128 + a 4-texel border each side) of mip Job.z at page (Job.x, Job.y), written into atlas
        // slot Job.w. Each texel is the ground at its centre's xz: the tile covering it (the tile grid), the tile's
        // weightmap through the bindless table, then Common/LandscapeSurface.glslh EvaluateLandscapeLayers - the
        // raster's and the hit group's own statement of the layer blend - with the layer maps fetched at the
        // texel's footprint. Stored: Color = sqrt(albedo / 2), Normal = (tangent normal xy * 0.5 + 0.5,
        // roughness), Occlusion = AO (Common/LandscapeVirtualTexture.glslh decodes).
        LocalSize(8, 8, 1);

        #include <Common/BindlessTextures.glslh>
        #include <Common/TerrainInstance.glslh>
        #include <Common/LandscapeSurface.glslh>

        // A tile's row: TerrainRayHit.glslh LandscapeRayHitRowRef's layout (C++: RayTracing::LandscapeRayHitRow).
        struct LandscapeVirtualRow
        {
            TerrainInstance   Tile;
            vec4              Tint;
            uvec4             Weightmap; // x = weightmap bindless slot (0 = none), y = its sampler, z = the maps' sampler
            LandscapeMaterial MaterialValues;
            uvec4             LayerMaps[8]; // input k's maps' bindless slots: x albedo, y normal, z ARM, w height
        };
        ReadBuffer(0) LandscapeVirtualRows
        {
            LandscapeVirtualRow VirtualRows[];
        };
        // x, y = the page, z = its mip, w = the atlas slot.
        ReadBuffer(1) LandscapeVirtualJobs
        {
            uvec4 VirtualJobs[];
        };
        // [0] cells a side, [1] cell size (cm, float bits), [2..3] 0, then row + 1 per cell (0 = no tile).
        ReadBuffer(2) LandscapeVirtualTiles
        {
            uint VirtualTiles[];
        };
        // The page table's header (LandscapeVirtualTexture::AddPasses): [0..2] origin x, z and extent (cm, float
        // bits), [6] the atlas side in pages, [24] log2 of the mip0 side in texels, [25] the view's pixel angle.
        ReadBuffer(3) LandscapeVirtualPageTable
        {
            uint VirtualPageTable[];
        };
        layout( binding = 4, rgb10_a2 ) writeonly uniform image2D LandscapeVirtualColorAtlas;
        layout( binding = 5, rgb10_a2 ) writeonly uniform image2D LandscapeVirtualNormalAtlas;
        layout( binding = 6, r8 ) writeonly uniform image2D LandscapeVirtualOcclusionAtlas;

        const uint kVirtualPageTexels = 128u;
        const uint kVirtualPageBorder = 4u;
        const uint kVirtualPageStride = 136u;

        uint  g_VirtualRow      = 0u;
        float g_VirtualTexelLod = 0.0; // log2 of the texel's footprint (cm)

        // Common/LandscapeSurface.glslh's fetch: the bindless image input k's array slice was copied from (the hit
        // group's table), at the level whose texel matches the page texel's footprint (BindlessTextures.glslh
        // RayHitTextureLod's statement, with the texel's footprint for the cone's).
        vec4 LandscapeLayerMapSample( int k, int kind, float slice, vec2 uv, float uvPerCm )
        {
            const uvec4           maps = VirtualRows[g_VirtualRow].LayerMaps[k];
            const uint            slot = kind == 0 ? maps.x : ( kind == 1 ? maps.y : ( kind == 2 ? maps.z : maps.w ) );
            const BindlessTexture map  = BindlessTexture( slot, VirtualRows[g_VirtualRow].Weightmap.z, 0u, 0u );
            const ivec2           size = BindlessTextureSize( map, 0 );
            const float lod = g_VirtualTexelLod + 0.5 * log2( float( max( size.x * size.y, 1 ) ) ) + log2( uvPerCm );
            return SampleBindlessLod( map, uv, max( lod, 0.0 ) );
        }

        // The tile's two weightmap pages at @p worldPosition, as the raster samples them (TerrainSurface.glslh).
        void LandscapeVirtualWeights( TerrainInstance tile, uvec4 weightmap, vec3 worldPosition, out vec4 w0,
                                      out vec4 w1 )
        {
            w0 = vec4( 0.0 );
            w1 = vec4( 0.0 );
            if ( weightmap.x == 0u || !LandscapeTileHasLayers( tile ) )
                return;
            const BindlessTexture map   = BindlessTexture( weightmap.x, weightmap.y, 0u, 0u );
            const vec2            g     = LandscapeTileSampleCoord( tile, worldPosition );
            const vec2            size  = vec2( BindlessTextureSize( map, 0 ) );
            const float           pages = LandscapeTilePages( tile );
            if ( !LandscapeTilePageEmpty( tile, 0 ) )
                w0 = SampleBindlessLod( map, LandscapeWeightmapPageUV( g, 0.0, size, pages ), 0.0 );
            if ( pages > 1.5 && !LandscapeTilePageEmpty( tile, 1 ) )
                w1 = SampleBindlessLod( map, LandscapeWeightmapPageUV( g, 1.0, size, pages ), 0.0 );
        }

        void main()
        {
            const uvec2 t = gl_GlobalInvocationID.xy;
            if ( t.x >= kVirtualPageStride || t.y >= kVirtualPageStride )
                return;
            const uvec4 job  = VirtualJobs[gl_WorkGroupID.z];
            const uint  pool = VirtualPageTable[6];
            const ivec2 dst  = ivec2( uvec2( job.w % pool, job.w / pool ) * kVirtualPageStride + t );

            const vec2  origin     = vec2( uintBitsToFloat( VirtualPageTable[0] ), uintBitsToFloat( VirtualPageTable[1] ) );
            const float extent     = uintBitsToFloat( VirtualPageTable[2] );
            const float mip0Texel  = extent / float( 1u << VirtualPageTable[24] ); // cm
            const float pixelAngle = max( uintBitsToFloat( VirtualPageTable[25] ), 1.0e-6 );
            const float mipScale   = float( 1u << job.z );
            const float texelCm    = mip0Texel * mipScale;
            // The texel's centre, in mip0 texels from the square's origin, then on the ground.
            const vec2 inMip = vec2( job.xy * kVirtualPageTexels ) + vec2( t ) - float( kVirtualPageBorder ) + 0.5;
            const vec2 xz    = origin + inMip * texelCm;

            const uint  grid = VirtualTiles[0];
            const float cell = uintBitsToFloat( VirtualTiles[1] );
            const ivec2 c    = clamp( ivec2( floor( ( xz - origin ) / cell ) ), ivec2( 0 ), ivec2( int( grid ) - 1 ) );
            const uint  row  = VirtualTiles[4u + uint( c.y ) * grid + uint( c.x )];
            if ( row == 0u )
            {
                // No ground here (off the queued tiles): a flat white texel the G-buffer never draws.
                imageStore( LandscapeVirtualColorAtlas, dst, vec4( sqrt( 0.5 ), sqrt( 0.5 ), sqrt( 0.5 ), 1.0 ) );
                imageStore( LandscapeVirtualNormalAtlas, dst, vec4( 0.5, 0.5, 0.9, 1.0 ) );
                imageStore( LandscapeVirtualOcclusionAtlas, dst, vec4( 1.0 ) );
                return;
            }
            g_VirtualRow      = row - 1u;
            g_VirtualTexelLod = log2( texelCm );
            const TerrainInstance tile     = VirtualRows[g_VirtualRow].Tile;
            const vec3            position = vec3( xz.x, 0.0, xz.y );
            vec4                  w0;
            vec4                  w1;
            LandscapeVirtualWeights( tile, VirtualRows[g_VirtualRow].Weightmap, position, w0, w1 );
            // The far tiling fades by distance: the distance at which the G-buffer picks this texel's mip.
            const float        viewDistance = texelCm * 1.41421356 / pixelAngle;
            const TerrainLayer l = EvaluateLandscapeLayers( tile, VirtualRows[g_VirtualRow].MaterialValues, position,
                                                            viewDistance, w0, w1 );
            const vec3 tn = normalize( l.Normal );
            imageStore( LandscapeVirtualColorAtlas, dst,
                        vec4( sqrt( clamp( l.Albedo * 0.5, vec3( 0.0 ), vec3( 1.0 ) ) ), 1.0 ) );
            imageStore( LandscapeVirtualNormalAtlas, dst,
                        vec4( clamp( tn.xy * 0.5 + 0.5, vec2( 0.0 ), vec2( 1.0 ) ), clamp( l.Roughness, 0.0, 1.0 ), 1.0 ) );
            imageStore( LandscapeVirtualOcclusionAtlas, dst, vec4( clamp( l.AO, 0.0, 1.0 ) ) );
        }
    }
}
