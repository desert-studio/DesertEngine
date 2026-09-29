// DesertAsset {"Kind":"Shader","Guid":"0b8c0b888307fc59733a35328241c38c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "FXAA"
{
    Fragment
    {
        // FXAA (Fast Approximate Anti-Aliasing), the classic compact luma-based variant. Runs on the LDR,
        // gamma-encoded tonemapped image. Texel size is derived from textureSize() — no UB needed. This is a
        // deferred-friendly, post-process-only technique (no MSAA, no motion vectors).

        In(0)  vec2 v_TexCoord;
        Uniform(2) sampler2D u_InputTexture;
        Out(0) vec4 oColor;

        #define FXAA_SPAN_MAX   8.0
        #define FXAA_REDUCE_MUL (1.0 / 8.0)
        #define FXAA_REDUCE_MIN (1.0 / 128.0)

        float luma( vec3 c )
        {
            return dot( c, vec3( 0.299, 0.587, 0.114 ) );
        }

        void main()
        {
            vec2 inverseVP = 1.0 / vec2( textureSize( u_InputTexture, 0 ) );

            vec3 rgbM  = texture( u_InputTexture, v_TexCoord ).rgb;
            vec3 rgbNW = textureOffset( u_InputTexture, v_TexCoord, ivec2( -1, -1 ) ).rgb;
            vec3 rgbNE = textureOffset( u_InputTexture, v_TexCoord, ivec2(  1, -1 ) ).rgb;
            vec3 rgbSW = textureOffset( u_InputTexture, v_TexCoord, ivec2( -1,  1 ) ).rgb;
            vec3 rgbSE = textureOffset( u_InputTexture, v_TexCoord, ivec2(  1,  1 ) ).rgb;

            float lumaNW = luma( rgbNW );
            float lumaNE = luma( rgbNE );
            float lumaSW = luma( rgbSW );
            float lumaSE = luma( rgbSE );
            float lumaM  = luma( rgbM );

            float lumaMin = min( lumaM, min( min( lumaNW, lumaNE ), min( lumaSW, lumaSE ) ) );
            float lumaMax = max( lumaM, max( max( lumaNW, lumaNE ), max( lumaSW, lumaSE ) ) );

            // Edge direction.
            vec2 dir;
            dir.x = -( ( lumaNW + lumaNE ) - ( lumaSW + lumaSE ) );
            dir.y =  ( ( lumaNW + lumaSW ) - ( lumaNE + lumaSE ) );

            float dirReduce = max( ( lumaNW + lumaNE + lumaSW + lumaSE ) * ( 0.25 * FXAA_REDUCE_MUL ), FXAA_REDUCE_MIN );
            float rcpDirMin = 1.0 / ( min( abs( dir.x ), abs( dir.y ) ) + dirReduce );
            dir = clamp( dir * rcpDirMin, vec2( -FXAA_SPAN_MAX ), vec2( FXAA_SPAN_MAX ) ) * inverseVP;

            vec3 rgbA = 0.5 * ( texture( u_InputTexture, v_TexCoord + dir * ( 1.0 / 3.0 - 0.5 ) ).rgb +
                                texture( u_InputTexture, v_TexCoord + dir * ( 2.0 / 3.0 - 0.5 ) ).rgb );
            vec3 rgbB = rgbA * 0.5 + 0.25 * ( texture( u_InputTexture, v_TexCoord + dir * -0.5 ).rgb +
                                              texture( u_InputTexture, v_TexCoord + dir *  0.5 ).rgb );

            float lumaB = luma( rgbB );
            if ( lumaB < lumaMin || lumaB > lumaMax )
                oColor = vec4( rgbA, 1.0 );
            else
                oColor = vec4( rgbB, 1.0 );
        }
    }

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/QuadTextureCoords.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord  = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];
            gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }
}
