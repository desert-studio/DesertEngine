// DesertAsset {"Kind":"Shader","Guid":"059ca771334649be92be3817c0de58a8","Versions":{"SHDR":1},"Dependencies":[]}
// "Grass: Blade Ray Tracing Vertices" (GrassBladeRayTracing.cpp). UE builds a ray-tracing BLAS from the geometry its
// vertex factory evaluates, without WPO unless it is enabled: here ONE blade per (variety, level), shaped by the
// raster's own BladeAt (Mesh/Surface/GrassBlade.glslh) for an identity instance with the median per-blade draws and
// no wind, written in blade space. Every TLAS row of the variety's level instances it through the raster's mat4,
// whose affine part places, scales and orients the blade exactly as the vertex stage does; the per-blade curvature
// and bend draws and the wind are not traced (the shared BLAS is static).
//
// Input: the blade strip's level as the raster draws it (GrassRenderer::BladeStrip, MakeGrassBladeStripLevel):
// Desert::Vertex with position (side -1/+1, height share t, 0), and its indices. Output: the shaped vertices
// (Desert::Vertex: position, the blade's shading normal, tangent = across, bitangent = up the blade, the strip's
// uv) and the indices copied, the build inputs of the level's BLAS. One thread per vertex and per index.
Shader "GrassBladeRayTracingVertices"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Mesh/Surface/GrassBlade.glslh>

        // The variety's shape, the same GrassBladeShapeGpu the raster's GrassBladeShape buffer holds.
        ReadBuffer(0) GrassBladeShape
        {
            GrassBladeShapeData Blade;
        };
        ReadBuffer(1) GrassBladeStripVertices
        {
            uint StripVertices[]; // Desert::Vertex, 14 words each
        };
        ReadBuffer(2) GrassBladeStripIndices
        {
            uint StripIndices[];
        };
        Buffer(3) GrassBladeRayTracingVertexData
        {
            uint Vertices[]; // Desert::Vertex, 14 words each
        };
        Buffer(4) GrassBladeRayTracingIndexData
        {
            uint Indices[];
        };

        PushConstant PushConstants
        {
            uvec4 Counts; // x = the level's vertices, y = its indices
        };

        const uint kVertexWords = 14u;
        // The per-blade curvature draw (instance column 1's w) of the median blade: a lean draw of 1/2 (the
        // variety's Curvature, CurvatureSpread cancels to 1/256 of itself) and a bend draw of 1/2 (its low bits:
        // fract(w * 256) = 1/2).
        const float kMedianCurvatureDraw = 0.5 + 0.5 / 256.0;

        void main()
        {
            const uint i = gl_GlobalInvocationID.x;
            if ( i < Counts.y )
                Indices[i] = StripIndices[i];
            if ( i >= Counts.x )
                return;
            const uint  r    = i * kVertexWords;
            const float side = uintBitsToFloat( StripVertices[r + 0u] );
            const float t    = uintBitsToFloat( StripVertices[r + 1u] );
            const mat4  model =
                 mat4( vec4( 1.0, 0.0, 0.0, 0.0 ), vec4( 0.0, 1.0, 0.0, kMedianCurvatureDraw ),
                       vec4( 0.0, 0.0, 1.0, 0.0 ), vec4( 0.0, 0.0, 0.0, 1.0 ) );
            const BladePoint blade  = BladeAt( Blade, model, side, t, vec4( 0.0 ), vec4( 0.0 ) );
            const vec3       normal = BladeNormal( blade, side );
            const vec3       along  = normalize( blade.Along );
            const float      words[12] = float[12]( blade.Position.x, blade.Position.y, blade.Position.z, normal.x,
                                                    normal.y, normal.z, blade.Right.x, blade.Right.y, blade.Right.z,
                                                    along.x, along.y, along.z );
            for ( uint w = 0u; w < 12u; ++w )
                Vertices[r + w] = floatBitsToUint( words[w] );
            Vertices[r + 12u] = StripVertices[r + 12u];
            Vertices[r + 13u] = StripVertices[r + 13u];
        }
    }
}
