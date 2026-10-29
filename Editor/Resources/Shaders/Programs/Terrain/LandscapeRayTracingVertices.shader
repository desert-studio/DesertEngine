// DesertAsset {"Kind":"Shader","Guid":"3f6c1a9e7b2d4e58a1c09d7e5b4f2a63","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LandscapeRayTracingVertices"
{
    Compute
    {
        // "Landscape: Ray Tracing Vertices" (LandscapeRayTracing.cpp). UE FLandscapeRayTracingImpl: the tile's
        // ray-tracing geometry at the VIEW's raster LOD, written from the heightmap by the same vertex evaluation
        // the raster stage runs (Programs/Terrain/LandscapeVertexEval.glslh), then built / refit into the tile's
        // BLAS by "Landscape: Ray Tracing Geometry".
        //
        // Output, per tile: the LOD grid's (cells + 1)^2 vertices as Desert::Vertex (position, normal, tangent,
        // bitangent, uv = the tile-local sample coordinate the RT alpha test reads the hole mask at), plus ONE
        // vertex whose x is NaN; and two index ranges of 6 indices per cell - [0, 6 cells^2) the OPAQUE geometry,
        // [6 cells^2, 12 cells^2) the MASKED geometry (non-opaque: the alpha test reads the visibility layer). Each
        // cell's two triangles live in exactly one range; the other range points them at the NaN vertex, which
        // makes them inactive triangles (Vulkan: a NaN x deactivates a triangle). A cell whose every reachable
        // sample is a hole is inactive in both: the bilinear of hole weights is a hole, so nothing is lost.
        LocalSize(64, 1, 1);

        #include <Common/TerrainInstance.glslh>
        // The raster's own TerrainInstance rows (TerrainRenderer::PrepareFrame), one per tile of this pass.
        ReadBuffer(0) LandscapeRayTracingRows
        {
            TerrainInstance Terrains[];
        };
        // Shared sampler states (UE shared samplers): each input below is a separate texture read through one of
        // these, bound by the pass by name and numbered by the program build (ShaderMapBuild.cpp
        // AssignSharedSamplerBindings).
        Uniform sampler PointClampSampler;
        Uniform(1) texture2D HeightmapTexture;
        #define HeightmapTexture sampler2D(HeightmapTexture, PointClampSampler)
        // The tile's RGBA8 weight pages (LandscapeWeightDraw); the engine fallback when it has none (Tile.w = 0).
        Uniform(2) texture2D WeightmapTexture;
        #define WeightmapTexture sampler2D(WeightmapTexture, PointClampSampler)

        Buffer(3) LandscapeRayTracingVertexData
        {
            uint Vertices[]; // Desert::Vertex, 14 words each
        };
        Buffer(4) LandscapeRayTracingIndexData
        {
            uint Indices[];
        };

        PushConstant PushConstants
        {
            uvec4 Tile; // x = the tile's row, y = grid vertices per side (cells + 1), z = cells per side, w = 1 when a weightmap is bound
        };

        #define CurrentTerrain Terrains[Tile.x]

        #include <Common/LandscapeHeight.glslh>
        #include <Common/LandscapeLod.glslh>
        #include <Common/LandscapeWeights.glslh>
        #include <Programs/Terrain/LandscapeVertexEval.glslh>

        const uint kVertexWords = 14u;

        void WriteVertex( uint index, vec3 position, vec3 normal, vec3 tangent, vec3 bitangent, vec2 uv )
        {
            uint w = index * kVertexWords;
            Vertices[w + 0u]  = floatBitsToUint( position.x );
            Vertices[w + 1u]  = floatBitsToUint( position.y );
            Vertices[w + 2u]  = floatBitsToUint( position.z );
            Vertices[w + 3u]  = floatBitsToUint( normal.x );
            Vertices[w + 4u]  = floatBitsToUint( normal.y );
            Vertices[w + 5u]  = floatBitsToUint( normal.z );
            Vertices[w + 6u]  = floatBitsToUint( tangent.x );
            Vertices[w + 7u]  = floatBitsToUint( tangent.y );
            Vertices[w + 8u]  = floatBitsToUint( tangent.z );
            Vertices[w + 9u]  = floatBitsToUint( bitangent.x );
            Vertices[w + 10u] = floatBitsToUint( bitangent.y );
            Vertices[w + 11u] = floatBitsToUint( bitangent.z );
            Vertices[w + 12u] = floatBitsToUint( uv.x );
            Vertices[w + 13u] = floatBitsToUint( uv.y );
        }

        // Grid vertex (gx, gz) of the tile's LOD grid, placed as LandscapeLodGridCoord places a cell corner.
        LandscapeMorphedVertex GridVertex( int gx, int gz )
        {
            int quads  = int( CurrentTerrain.LandscapeTile.z + 0.5 );
            int stride = LandscapeLodStride( int( CurrentTerrain.Params.w + 0.5 ) );
            return LandscapeEvaluateVertex( min( gx * stride, quads ), min( gz * stride, quads ) );
        }

        // Whether tile sample (x, z) is a hole: the surface's own test (LandscapeIsHole) on the weight pages.
        bool SampleIsHole( int x, int z )
        {
            float pages = CurrentTerrain.Params2.z;
            int   rows  = int( float( textureSize( WeightmapTexture, 0 ).y ) / max( pages, 1.0 ) + 0.5 );
            vec4  w0    = texelFetch( WeightmapTexture, ivec2( x, z ), 0 );
            vec4  w1    = pages > 1.5 ? texelFetch( WeightmapTexture, ivec2( x, z + rows ), 0 ) : vec4( 0.0 );
            return LandscapeIsHole( LandscapeVisibility( w0, w1, CurrentTerrain.Params.z ) );
        }

        void main()
        {
            uint i     = gl_GlobalInvocationID.x;
            uint side  = Tile.y;
            uint cells = Tile.z;
            uint nanVertex = side * side;

            if ( i < nanVertex )
            {
                LandscapeMorphedVertex v = GridVertex( int( i % side ), int( i / side ) );
                vec2 d = LandscapeVertexGradient( v );
                WriteVertex( i, v.WorldPos, normalize( vec3( -d.x, 1.0, -d.y ) ), normalize( vec3( 1.0, d.x, 0.0 ) ),
                             normalize( vec3( 0.0, d.y, 1.0 ) ), v.Local );
            }
            if ( i == 0u )
                WriteVertex( nanVertex, vec3( uintBitsToFloat( 0x7fc00000u ) ), vec3( 0.0, 1.0, 0.0 ),
                             vec3( 1.0, 0.0, 0.0 ), vec3( 0.0, 0.0, 1.0 ), vec2( 0.0 ) );

            if ( i >= cells * cells )
                return;
            int cx = int( i % cells );
            int cz = int( i / cells );

            // Which samples the cell's two triangles can cover: the bounding box of its four MORPHED corners (a
            // corner on a coarser border snaps further than the cell's own grid). The visibility between samples
            // is the bilinear of the samples around it, so the box's samples decide the whole cell exactly.
            bool anyHole = false;
            bool allHole = false;
            if ( Tile.w == 1u && CurrentTerrain.Params.z > 0.5 )
            {
                vec2 lo = vec2( 1e9 );
                vec2 hi = vec2( -1e9 );
                for ( int corner = 0; corner < 4; ++corner )
                {
                    vec2 local = GridVertex( cx + ( corner & 1 ), cz + ( corner >> 1 ) ).Local;
                    lo = min( lo, local );
                    hi = max( hi, local );
                }
                int last = int( CurrentTerrain.LandscapeTile.z + 0.5 );
                int x0   = clamp( int( floor( lo.x ) ), 0, last );
                int z0   = clamp( int( floor( lo.y ) ), 0, last );
                int x1   = clamp( int( ceil( hi.x ) ), 0, last );
                int z1   = clamp( int( ceil( hi.y ) ), 0, last );
                allHole  = true;
                for ( int z = z0; z <= z1; ++z )
                {
                    for ( int x = x0; x <= x1; ++x )
                    {
                        bool hole = SampleIsHole( x, z );
                        anyHole   = anyHole || hole;
                        allHole   = allHole && hole;
                    }
                }
            }

            uint opaqueBase = i * 6u;
            uint maskedBase = cells * cells * 6u + i * 6u;
            for ( int k = 0; k < 6; ++k )
            {
                uint vertex = uint( cz + LandscapeLodCornerZ( k ) ) * side + uint( cx + LandscapeLodCornerX( k ) );
                Indices[opaqueBase + uint( k )] = !anyHole ? vertex : nanVertex;
                Indices[maskedBase + uint( k )] = ( anyHole && !allHole ) ? vertex : nanVertex;
            }
        }
    }
}
