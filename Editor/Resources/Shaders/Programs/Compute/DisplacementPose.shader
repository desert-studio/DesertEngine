// DesertAsset {"Kind":"Shader","Guid":"57a5195ae17a4f128f97a0a774a2a9a2","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DisplacementPose"
{
    Compute
    {
        // "Displacement: Pose" (DisplacementCacheSkinned.cpp, CONTRACT-DISPLACE §9.8). One invocation per WELDED
        // vertex of a skinned mesh's displacement topology, every frame, after "SkinCache: Skin" wrote the base mesh's
        // posed streams: the weld's posed position is the mean of its distinct copies' posed positions, its posed
        // direction the normalised sum of their posed normals - the rule BuildDisplacementTopology applies to the
        // rest copies (WeldPosition, WeldDirection), so in the bind pose this pass reproduces the rest topology and
        // the skinned surface equals the static one. Copies of one weld share one result: a UV seam stays closed.
        LocalSize(64, 1, 1);

        // The base mesh's skin-cache streams (RayTracingSkinCache.hpp, Common/SkinCacheStreams.glslh), V = Counts.y
        // vertices: current positions [0, 3V), previous [3V, 6V), tangent frame [6V, 15V) (normal first).
        Buffer(0) DisplacementBaseStreams
        {
            float BaseStreamValues[];
        };
        // The weld -> copies CSR (DisplacementCache::SkinnedTopologyWords): CopyOffset [0, W + 1) at word 0, the
        // distinct mesh vertices of every weld after it (at Offsets.x).
        Buffer(1) DisplacementWeldCopies
        {
            uint WeldCopyWords[];
        };
        // Counts.x = welds, Counts.y = base vertices; Offsets.x = the first copy word.
        Buffer(2) DisplacementDispatch
        {
            uvec4 Counts;
            uvec4 Offsets;
            vec4  Parameters;
        };
        // Out: per weld the posed position (3 floats) and the posed direction (3 floats).
        Buffer(3) DisplacementPosedWelds
        {
            float PosedWeldValues[];
        };

        vec3 BaseFetch( uint first )
        {
            return vec3( BaseStreamValues[first], BaseStreamValues[first + 1u], BaseStreamValues[first + 2u] );
        }

        void main()
        {
            const uint weld = gl_GlobalInvocationID.x;
            if ( weld >= Counts.x )
                return;
            const uint first = WeldCopyWords[weld];
            const uint last  = WeldCopyWords[weld + 1u];
            vec3       position  = vec3( 0.0 );
            vec3       direction = vec3( 0.0 );
            for ( uint c = first; c < last; ++c )
            {
                const uint vertex = WeldCopyWords[Offsets.x + c];
                position += BaseFetch( vertex * 3u );
                direction += BaseFetch( Counts.y * 6u + vertex * 9u );
            }
            position /= float( max( last - first, 1u ) );
            // A pose that folds the copies' normals onto each other leaves no direction: the point does not move
            // rather than move along a degenerate axis (the rest topology refused such a weld at cook).
            const float len = length( direction );
            direction       = len > 1e-6 ? direction / len : vec3( 0.0 );
            const uint out0 = weld * 6u;
            PosedWeldValues[out0 + 0u] = position.x;
            PosedWeldValues[out0 + 1u] = position.y;
            PosedWeldValues[out0 + 2u] = position.z;
            PosedWeldValues[out0 + 3u] = direction.x;
            PosedWeldValues[out0 + 4u] = direction.y;
            PosedWeldValues[out0 + 5u] = direction.z;
        }
    }
}
