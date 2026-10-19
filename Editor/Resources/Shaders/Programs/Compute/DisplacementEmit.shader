// DesertAsset {"Kind":"Shader","Guid":"af46fe8122b94ca9816e61aa6cc5931b","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DisplacementEmit"
{
    Compute
    {
        // "Displacement: Emit" (DisplacementCache.cpp). One invocation per output vertex of a displaced primitive
        // (Geometry/DisplacementTessellation.hpp DisplacementVertex): the attributes are interpolated in the vertex's
        // source triangle (normal, tangent and bitangent renormalised - as in UE the shading normal stays the
        // interpolated base normal and the material's normal map adds the detail), the position is its point's
        // displaced position ("Displacement: Heights"). The output is a Desert::Vertex buffer the raster binds as its
        // vertex stream and the BLAS reads as its build input.
        LocalSize(64, 1, 1);

        // Desert::Vertex as 14 words (Geometry/MeshTypes.hpp).
        Buffer(0) DisplacementSourceVertices
        {
            uint SourceVertexWords[];
        };
        // Desert::MeshVertexStreams as 3 words (Color RGBA8, UV1 x, UV1 y); read only when Offsets.w == 1.
        Buffer(1) DisplacementSourceStreams
        {
            uint SourceStreamWords[];
        };
        // As in DisplacementHeights.shader: CornerVertex [0, 3T), ...
        Buffer(2) DisplacementTopology
        {
            uint TopologyWords[];
        };
        // DisplacementVertex, 3 words: triangle, barycentrics (b0 | b1 << 16), point.
        Buffer(3) DisplacementVertices
        {
            uint VertexRecordWords[];
        };
        Buffer(4) DisplacementPositions
        {
            float Positions[];
        };
        // Counts.x = output vertices; Offsets.w = 1 when the source (and the output) has streams.
        Buffer(5) DisplacementDispatch
        {
            uvec4 Counts;
            uvec4 Offsets;
            vec4  Parameters;
        };
        // Out: Desert::Vertex, 14 words per vertex.
        Buffer(6) DisplacementOutputVertices
        {
            float OutputVertexValues[];
        };
        // Out: Desert::MeshVertexStreams, 3 words per vertex (written when Offsets.w == 1).
        Buffer(7) DisplacementOutputStreams
        {
            uint OutputStreamWords[];
        };

        const float kBarycentricMax = 32768.0;

        float SourceFloat( uint vertex, uint word )
        {
            return uintBitsToFloat( SourceVertexWords[vertex * 14u + word] );
        }

        void main()
        {
            const uint vertex = gl_GlobalInvocationID.x;
            if ( vertex >= Counts.x )
                return;
            const uint  triangle     = VertexRecordWords[vertex * 3u + 0u];
            const uint  barycentrics = VertexRecordWords[vertex * 3u + 1u];
            const uint  point        = VertexRecordWords[vertex * 3u + 2u];
            const float w0           = float( barycentrics & 0xffffu ) / kBarycentricMax;
            const float w1           = float( barycentrics >> 16u ) / kBarycentricMax;
            const float w2           = 1.0 - w0 - w1;
            const uint  v0           = TopologyWords[triangle * 3u + 0u];
            const uint  v1           = TopologyWords[triangle * 3u + 1u];
            const uint  v2           = TopologyWords[triangle * 3u + 2u];

            const uint out0 = vertex * 14u;
            OutputVertexValues[out0 + 0u] = Positions[point * 3u + 0u];
            OutputVertexValues[out0 + 1u] = Positions[point * 3u + 1u];
            OutputVertexValues[out0 + 2u] = Positions[point * 3u + 2u];
            // Normal (3), tangent (6), bitangent (9): interpolated, renormalised.
            for ( uint frame = 3u; frame <= 9u; frame += 3u )
            {
                vec3 value = vec3( SourceFloat( v0, frame ), SourceFloat( v0, frame + 1u ), SourceFloat( v0, frame + 2u ) ) * w0 +
                             vec3( SourceFloat( v1, frame ), SourceFloat( v1, frame + 1u ), SourceFloat( v1, frame + 2u ) ) * w1 +
                             vec3( SourceFloat( v2, frame ), SourceFloat( v2, frame + 1u ), SourceFloat( v2, frame + 2u ) ) * w2;
                const float len = length( value );
                value           = len > 0.0 ? value / len : value;
                OutputVertexValues[out0 + frame + 0u] = value.x;
                OutputVertexValues[out0 + frame + 1u] = value.y;
                OutputVertexValues[out0 + frame + 2u] = value.z;
            }
            const vec2 uv = vec2( SourceFloat( v0, 12u ), SourceFloat( v0, 13u ) ) * w0 +
                            vec2( SourceFloat( v1, 12u ), SourceFloat( v1, 13u ) ) * w1 +
                            vec2( SourceFloat( v2, 12u ), SourceFloat( v2, 13u ) ) * w2;
            OutputVertexValues[out0 + 12u] = uv.x;
            OutputVertexValues[out0 + 13u] = uv.y;

            if ( Offsets.w != 0u )
            {
                const vec4 colour = unpackUnorm4x8( SourceStreamWords[v0 * 3u] ) * w0 +
                                    unpackUnorm4x8( SourceStreamWords[v1 * 3u] ) * w1 +
                                    unpackUnorm4x8( SourceStreamWords[v2 * 3u] ) * w2;
                const vec2 uv1 =
                     vec2( uintBitsToFloat( SourceStreamWords[v0 * 3u + 1u] ),
                           uintBitsToFloat( SourceStreamWords[v0 * 3u + 2u] ) ) * w0 +
                     vec2( uintBitsToFloat( SourceStreamWords[v1 * 3u + 1u] ),
                           uintBitsToFloat( SourceStreamWords[v1 * 3u + 2u] ) ) * w1 +
                     vec2( uintBitsToFloat( SourceStreamWords[v2 * 3u + 1u] ),
                           uintBitsToFloat( SourceStreamWords[v2 * 3u + 2u] ) ) * w2;
                OutputStreamWords[vertex * 3u + 0u] = packUnorm4x8( colour );
                OutputStreamWords[vertex * 3u + 1u] = floatBitsToUint( uv1.x );
                OutputStreamWords[vertex * 3u + 2u] = floatBitsToUint( uv1.y );
            }
        }
    }
}
