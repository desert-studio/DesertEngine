// DesertAsset {"Kind":"Shader","Guid":"e8b4cb4c2118456bbaccd9e96f3f17be","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DisplacementHeights"
{
    Compute
    {
        // "Displacement: Heights" (DisplacementCache.cpp). One invocation per displaced POINT of a primitive's
        // tessellation (Geometry/DisplacementTessellation.hpp DisplacementPoint): the point's undisplaced position and
        // direction are interpolated over its welded vertices in their canonical order, its displacement is the AVERAGE
        // of (height - centre) * scale over its samples - every triangle corner that touches the point, each read with
        // its own triangle's UV and its own submesh's material - and the displaced position is written for the emit
        // pass. A point on a welded edge or vertex has one record shared by every cluster that emits it, evaluated by
        // this one instruction sequence, so a UV seam or a material boundary cannot open a crack.
        LocalSize(64, 1, 1);

        #include <Common/BindlessTextures.glslh>
        #include <Common/SurfaceInputs.glslh>

        // The source mesh's vertices, Counts.z words each: Desert::Vertex (14: Position 0-2, Normal 3-5, Tangent 6-8,
        // Bitangent 9-11, TexCoord 12-13; Geometry/MeshTypes.hpp) or a skinned mesh's rest SkinnedVertex (22: the
        // same 14 words, then bone ids and weights).
        Buffer(0) DisplacementSourceVertices
        {
            uint SourceVertexWords[];
        };
        // The source mesh's optional streams: Desert::MeshVertexStreams as 3 words (Color RGBA8, UV1 x, UV1 y).
        Buffer(1) DisplacementSourceStreams
        {
            uint SourceStreamWords[];
        };
        // The mesh's displacement topology, end to end: CornerVertex [0, 3T), TriangleSubmesh at Offsets.x,
        // WeldPosition (3 floats per weld) at Offsets.y, WeldDirection (3 floats per weld) at Offsets.z.
        Buffer(2) DisplacementTopology
        {
            uint TopologyWords[];
        };
        // One DisplacementMaterialRecord per submesh, 12 words (DisplacementCache.hpp): texture slot (~0 = no
        // height), sampler slot, channel, UV set, scale cm, centre, tiling xy, offset xy, rotation, has transform.
        Buffer(3) DisplacementMaterials
        {
            uint MaterialWords[];
        };
        // DisplacementPoint, 7 words: welds 0-2, weights (b0 | b1 << 16), sample offset, sample count, level.
        Buffer(4) DisplacementPoints
        {
            uint PointWords[];
        };
        // DisplacementSample, 2 words: triangle, barycentrics (b0 | b1 << 16).
        Buffer(5) DisplacementSamples
        {
            uint SampleWords[];
        };
        // Counts.x = points, Counts.z = words per source vertex;
        // Offsets = the topology sections (x, y, z as above), Offsets.w = 1 when the source has
        // streams; Parameters.x = the mesh's level-0 edge length (cm).
        Buffer(6) DisplacementDispatch
        {
            uvec4 Counts;
            uvec4 Offsets;
            vec4  Parameters;
        };
        // Out: per point the displaced position (3 floats) and the displacement along its direction (cm): the skinned
        // pass moves the point with the pose and re-applies that displacement along the posed direction.
        Buffer(7) DisplacementPositions
        {
            float Positions[];
        };

        const float kBarycentricMax = 32768.0;

        vec3 SourceVec3( uint vertex, uint word )
        {
            const uint first = vertex * Counts.z + word;
            return vec3( uintBitsToFloat( SourceVertexWords[first] ),
                         uintBitsToFloat( SourceVertexWords[first + 1u] ),
                         uintBitsToFloat( SourceVertexWords[first + 2u] ) );
        }

        vec2 SourceUV( uint vertex, uint uvSet )
        {
            if ( uvSet == 0u || Offsets.w == 0u )
                return vec2( uintBitsToFloat( SourceVertexWords[vertex * Counts.z + 12u] ),
                             uintBitsToFloat( SourceVertexWords[vertex * Counts.z + 13u] ) );
            return vec2( uintBitsToFloat( SourceStreamWords[vertex * 3u + 1u] ),
                         uintBitsToFloat( SourceStreamWords[vertex * 3u + 2u] ) );
        }

        vec3 TopologyVec3( uint section, uint index )
        {
            const uint first = section + index * 3u;
            return vec3( uintBitsToFloat( TopologyWords[first] ),
                         uintBitsToFloat( TopologyWords[first + 1u] ),
                         uintBitsToFloat( TopologyWords[first + 2u] ) );
        }

        // (height - centre) * scale of one sample: the material of the sample triangle's submesh at the triangle's
        // own UV, at the mip whose texel matches the diced edge of @p level.
        float SampleDisplacement( uint triangle, uint barycentrics, uint level )
        {
            const uint submesh = TopologyWords[Offsets.x + triangle];
            const uint m       = submesh * 12u;
            const uint texture = MaterialWords[m + 0u];
            if ( texture == 0xffffffffu )
                return 0.0;
            const uint  samplerSlot = MaterialWords[m + 1u];
            const uint  channel     = min( MaterialWords[m + 2u], 3u );
            const uint  uvSet       = MaterialWords[m + 3u];
            const float scaleCm     = uintBitsToFloat( MaterialWords[m + 4u] );
            const float centre      = clamp( uintBitsToFloat( MaterialWords[m + 5u] ), 0.0, 1.0 );

            const float w0 = float( barycentrics & 0xffffu ) / kBarycentricMax;
            const float w1 = float( barycentrics >> 16u ) / kBarycentricMax;
            const float w2 = 1.0 - w0 - w1;
            const uint  v0 = TopologyWords[triangle * 3u + 0u];
            const uint  v1 = TopologyWords[triangle * 3u + 1u];
            const uint  v2 = TopologyWords[triangle * 3u + 2u];
            const vec2  uv0 = SourceUV( v0, uvSet );
            const vec2  uv1 = SourceUV( v1, uvSet );
            const vec2  uv2 = SourceUV( v2, uvSet );
            vec2        uv  = uv0 * w0 + uv1 * w1 + uv2 * w2;
            if ( MaterialWords[m + 11u] != 0u )
            {
                vec2 tiling = vec2( uintBitsToFloat( MaterialWords[m + 6u] ),
                                    uintBitsToFloat( MaterialWords[m + 7u] ) );
                if ( tiling.x <= 0.0 ) tiling.x = 1.0;
                if ( tiling.y <= 0.0 ) tiling.y = 1.0;
                const vec2 offset = vec2( uintBitsToFloat( MaterialWords[m + 8u] ),
                                          uintBitsToFloat( MaterialWords[m + 9u] ) );
                uv = SurfaceTransformUV( uv, offset, tiling, uintBitsToFloat( MaterialWords[m + 10u] ) );
            }

            // Texels per diced edge: the triangle's UV density (UV units per cm) times the edge length of the level.
            const vec3  p0      = SourceVec3( v0, 0u );
            const vec3  p1      = SourceVec3( v1, 0u );
            const vec3  p2      = SourceVec3( v2, 0u );
            const float area    = length( cross( p1 - p0, p2 - p0 ) );
            const vec2  e1      = uv1 - uv0;
            const vec2  e2      = uv2 - uv0;
            const float uvArea  = abs( e1.x * e2.y - e1.y * e2.x );
            const float uvPerCm = area > 0.0 ? sqrt( uvArea / area ) : 0.0;
            const ivec2 size    = textureSize( sampler2D( u_BindlessTextures[nonuniformEXT( texture )],
                                                          u_BindlessSamplers[nonuniformEXT( samplerSlot )] ), 0 );
            const float edgeCm  = Parameters.x * exp2( -float( level ) );
            const float texels  = float( max( size.x, size.y ) ) * uvPerCm * edgeCm;
            const float lod     = texels > 1.0 ? log2( texels ) : 0.0;

            const float height = SampleBindless( texture, samplerSlot, uv, lod )[channel];
            return ( height - centre ) * scaleCm;
        }

        void main()
        {
            const uint point = gl_GlobalInvocationID.x;
            if ( point >= Counts.x )
                return;
            const uint record  = point * 7u;
            const uint weld0   = PointWords[record + 0u];
            const uint weld1   = PointWords[record + 1u];
            const uint weld2   = PointWords[record + 2u];
            const uint weights = PointWords[record + 3u];
            const uint first   = PointWords[record + 4u];
            const uint count   = PointWords[record + 5u];
            const uint level   = PointWords[record + 6u];

            const float b0 = float( weights & 0xffffu ) / kBarycentricMax;
            const float b1 = float( weights >> 16u ) / kBarycentricMax;
            const float b2 = float( 32768u - ( weights & 0xffffu ) - ( weights >> 16u ) ) / kBarycentricMax;
            const vec3  position = TopologyVec3( Offsets.y, weld0 ) * b0 + TopologyVec3( Offsets.y, weld1 ) * b1 +
                                   TopologyVec3( Offsets.y, weld2 ) * b2;
            const vec3  direction = normalize( TopologyVec3( Offsets.z, weld0 ) * b0 +
                                               TopologyVec3( Offsets.z, weld1 ) * b1 +
                                               TopologyVec3( Offsets.z, weld2 ) * b2 );

            float displacement = 0.0;
            for ( uint s = 0u; s < count; ++s )
                displacement += SampleDisplacement( SampleWords[( first + s ) * 2u],
                                                    SampleWords[( first + s ) * 2u + 1u], level );
            displacement /= float( max( count, 1u ) );

            const vec3 displaced = position + direction * displacement;
            Positions[point * 4u + 0u] = displaced.x;
            Positions[point * 4u + 1u] = displaced.y;
            Positions[point * 4u + 2u] = displaced.z;
            Positions[point * 4u + 3u] = displacement;
        }
    }
}
