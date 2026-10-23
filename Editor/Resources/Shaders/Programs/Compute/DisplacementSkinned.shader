// DesertAsset {"Kind":"Shader","Guid":"f1b7f4646f514901aa9a116d1e2e3d26","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DisplacementSkinned"
{
    Compute
    {
        // "Displacement: Skinned" (DisplacementCacheSkinned.cpp, CONTRACT-DISPLACE §9.8). One invocation per vertex
        // of a skinned mesh's displaced twin, every frame, after "Displacement: Pose": the vertex's point is moved
        // with the pose - the canonical blend of its welds' posed positions - and displaced by its cooked
        // displacement (DisplacementHeights.shader, pose-independent: UV and material) along the blend of their
        // posed directions; the tangent frame is the posed frame interpolated in the source triangle. Written in
        // the skin cache's stream layout (RayTracingSkinCache.hpp) for V' = Counts.x twin vertices, so the raster,
        // the velocity pass and the BLAS read the displaced pose exactly as they read any skinned pose.
        LocalSize(64, 1, 1);

        const float kBarycentricMax = 32768.0;

        // The base mesh's skin-cache streams (V = Counts.y vertices), written this frame by "SkinCache: Skin".
        Buffer(0) DisplacementBaseStreams
        {
            float BaseStreamValues[];
        };
        // The twin's skinned records (DisplacementEmit.shader, Counts.w == 1), 7 words per twin vertex: welds 0-2,
        // weights (b0 | b1 << 16), displacement (float bits, cm), source triangle, barycentrics (b0 | b1 << 16).
        Buffer(1) DisplacementSkinRecords
        {
            uint SkinRecordWords[];
        };
        // The topology's CornerVertex, 3 words per LOD0 triangle (the mesh vertex of each corner).
        Buffer(2) DisplacementCorners
        {
            uint CornerWords[];
        };
        // "Displacement: Pose": 6 floats per weld (posed position, posed direction).
        Buffer(3) DisplacementPosedWelds
        {
            float PosedWeldValues[];
        };
        // Counts.x = twin vertices V', Counts.y = base vertices V, Counts.z = 1 when the displaced streams hold last
        // frame's pose (rotate it into the previous positions; 0 = a fresh entry writes the new pose into both).
        Buffer(4) DisplacementDispatch
        {
            uvec4 Counts;
            uvec4 Offsets;
            vec4  Parameters;
        };
        // Out: the twin's skin-cache streams, 15 floats per twin vertex in the three sections above.
        Buffer(5) DisplacementSkinnedStreams
        {
            float StreamValues[];
        };

        vec3 BaseFetch( uint first )
        {
            return vec3( BaseStreamValues[first], BaseStreamValues[first + 1u], BaseStreamValues[first + 2u] );
        }

        vec3 WeldFetch( uint weld, uint word )
        {
            const uint first = weld * 6u + word;
            return vec3( PosedWeldValues[first], PosedWeldValues[first + 1u], PosedWeldValues[first + 2u] );
        }

        void Store( uint first, vec3 value )
        {
            StreamValues[first + 0u] = value.x;
            StreamValues[first + 1u] = value.y;
            StreamValues[first + 2u] = value.z;
        }

        void main()
        {
            const uint vertex = gl_GlobalInvocationID.x;
            const uint count  = Counts.x;
            if ( vertex >= count )
                return;
            const uint record  = vertex * 7u;
            const uint weld0   = SkinRecordWords[record + 0u];
            const uint weld1   = SkinRecordWords[record + 1u];
            const uint weld2   = SkinRecordWords[record + 2u];
            const uint weights = SkinRecordWords[record + 3u];
            const float displacement = uintBitsToFloat( SkinRecordWords[record + 4u] );
            const uint triangle      = SkinRecordWords[record + 5u];
            const uint barycentrics  = SkinRecordWords[record + 6u];

            // The point, as DisplacementHeights.shader evaluates it at rest - the same canonical weights over the
            // same welds, so every vertex of one point (all clusters, both sides of a seam) gets the same bits.
            const float b0 = float( weights & 0xffffu ) / kBarycentricMax;
            const float b1 = float( weights >> 16u ) / kBarycentricMax;
            const float b2 = float( 32768u - ( weights & 0xffffu ) - ( weights >> 16u ) ) / kBarycentricMax;
            const vec3  position  = WeldFetch( weld0, 0u ) * b0 + WeldFetch( weld1, 0u ) * b1 +
                                    WeldFetch( weld2, 0u ) * b2;
            const vec3  blended   = WeldFetch( weld0, 3u ) * b0 + WeldFetch( weld1, 3u ) * b1 +
                                    WeldFetch( weld2, 3u ) * b2;
            const float blendLen  = length( blended );
            const vec3  direction = blendLen > 1e-6 ? blended / blendLen : vec3( 0.0 );
            const vec3  displaced = position + direction * displacement;

            if ( Counts.z != 0u )
                Store( count * 3u + vertex * 3u, vec3( StreamValues[vertex * 3u], StreamValues[vertex * 3u + 1u],
                                                       StreamValues[vertex * 3u + 2u] ) );
            else
                Store( count * 3u + vertex * 3u, displaced );
            Store( vertex * 3u, displaced );

            // Tangent frame: the source triangle's posed corner frames, interpolated and renormalised (as the
            // static Emit does at rest; the material's normal map adds the detail, §9.5).
            const float w0 = float( barycentrics & 0xffffu ) / kBarycentricMax;
            const float w1 = float( barycentrics >> 16u ) / kBarycentricMax;
            const float w2 = 1.0 - w0 - w1;
            const uint  v0 = CornerWords[triangle * 3u + 0u];
            const uint  v1 = CornerWords[triangle * 3u + 1u];
            const uint  v2 = CornerWords[triangle * 3u + 2u];
            const uint  baseFrame = Counts.y * 6u;
            for ( uint axis = 0u; axis < 3u; ++axis )
            {
                vec3 value = BaseFetch( baseFrame + v0 * 9u + axis * 3u ) * w0 +
                             BaseFetch( baseFrame + v1 * 9u + axis * 3u ) * w1 +
                             BaseFetch( baseFrame + v2 * 9u + axis * 3u ) * w2;
                const float len = length( value );
                value           = len > 0.0 ? value / len : value;
                Store( count * 6u + vertex * 9u + axis * 3u, value );
            }
        }
    }
}
