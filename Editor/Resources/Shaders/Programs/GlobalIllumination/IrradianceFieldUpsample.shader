// DesertAsset {"Kind":"Shader","Guid":"c55929a4fceb4296821b766a6a0ffe4a","Versions":{"SHDR":1},"Dependencies":[]}
// THE IRRADIANCE FIELD GATHER, 2/2 — UPSAMPLE (GIMethod::ProbeVolume): one thread per G-buffer pixel. The four
// half-resolution samples around the pixel (IrradianceFieldGather.shader: each the G-buffer pixel at its even
// corner), weighted bilinearly x by how close the sample's distance to the camera is to the pixel's and how far
// their normals agree — UE's screen probe upsample weights (CalculateUpsampleInterpolationWeights: plane distance
// and normal) at a 2-pixel spacing. Where no sample holds the pixel's surface (a silhouette one pixel wide) the
// sample nearest in distance answers alone, so a pixel never reads the surface behind it.
// Output: diffuse.rgb, specular.rgb at the G-buffer's extent (ViewTargetFormats::kGIFinalGather).
Shader "IrradianceFieldUpsample"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ReconstructPosition.glslh>

        ReadBuffer(0) GIFrameUB
        {
            GIFrame giFrame;
        };
        Uniform sampler PointClampSampler;
        Uniform(1) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(2) texture2D GBufferBTexture;
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        Uniform(3) texture2D GiHalfDiffuseTexture;
        #define GiHalfDiffuseTexture sampler2D(GiHalfDiffuseTexture, PointClampSampler)
        Uniform(4) texture2D GiHalfSpecularTexture;
        #define GiHalfSpecularTexture sampler2D(GiHalfSpecularTexture, PointClampSampler)
        layout(binding = 5, rgba16f) restrict writeonly uniform image2D GiDiffuseOutTexture;
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D GiSpecularOutTexture;
        Uniform(7) texture2D GiHalfBackfaceTexture;
        #define GiHalfBackfaceTexture sampler2D(GiHalfBackfaceTexture, PointClampSampler)
        layout(binding = 8, rgba16f) restrict writeonly uniform image2D GiBackfaceOutTexture;

        // The relative distance difference at which a sample's weight has fallen to 1/e (another surface).
        #define IF_DISTANCE_TOLERANCE 0.02

        void main()
        {
            const ivec2 pixel  = ivec2( gl_GlobalInvocationID.xy );
            const ivec2 extent = ivec2( giFrame.screen.xy );
            if ( any( greaterThanEqual( pixel, extent ) ) )
                return;
            const vec4 gb = texelFetch( GBufferBTexture, pixel, 0 );
            if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
            {
                imageStore( GiDiffuseOutTexture, pixel, vec4( 0.0 ) );
                imageStore( GiSpecularOutTexture, pixel, vec4( 0.0 ) );
                imageStore( GiBackfaceOutTexture, pixel, vec4( 0.0 ) );
                return;
            }
            const vec2  uv       = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  p        = ReconstructWorldPosition( uv, texelFetch( GBufferDepthTexture, pixel, 0 ).r,
                                                             giFrame.invViewProjection );
            const vec3  n        = normalize( gb.xyz );
            const float distance = max( length( giFrame.camera.xyz - p ), 1e-3 );

            // The half-resolution sample h sits on pixel 2h: the pixel's position on that grid.
            const ivec2 halfExtent = ( extent + 1 ) / 2;
            const vec2  c          = vec2( pixel ) * 0.5;
            const ivec2 base       = ivec2( floor( c ) );
            const vec2  f          = c - vec2( base );

            vec3  diffuse  = vec3( 0.0 );
            vec3  specular = vec3( 0.0 );
            vec3  backface = vec3( 0.0 );
            float total    = 0.0;
            float nearest  = 1e30;
            ivec2 nearestH = clamp( base, ivec2( 0 ), halfExtent - 1 );
            for ( int i = 0; i < 4; ++i )
            {
                const ivec2 o = ivec2( i & 1, i >> 1 );
                const ivec2 h = clamp( base + o, ivec2( 0 ), halfExtent - 1 );
                const vec4  d = texelFetch( GiHalfDiffuseTexture, h, 0 );
                if ( d.a <= 0.0 )
                    continue;
                const float gap = abs( d.a - distance );
                if ( gap < nearest )
                {
                    nearest  = gap;
                    nearestH = h;
                }
                const vec3  sampleN  = normalize( texelFetch( GBufferBTexture, min( h * 2, extent - 1 ), 0 ).xyz );
                const vec2  bilinear = mix( 1.0 - f, f, vec2( o ) );
                const float w        = bilinear.x * bilinear.y *
                                       exp( -gap / ( IF_DISTANCE_TOLERANCE * distance ) ) *
                                       max( dot( n, sampleN ), 0.0 );
                diffuse  += w * d.rgb;
                specular += w * texelFetch( GiHalfSpecularTexture, h, 0 ).rgb;
                backface += w * texelFetch( GiHalfBackfaceTexture, h, 0 ).rgb;
                total    += w;
            }
            if ( total > 1e-4 )
            {
                diffuse  /= total;
                specular /= total;
                backface /= total;
            }
            else
            {
                diffuse  = texelFetch( GiHalfDiffuseTexture, nearestH, 0 ).rgb;
                specular = texelFetch( GiHalfSpecularTexture, nearestH, 0 ).rgb;
                backface = texelFetch( GiHalfBackfaceTexture, nearestH, 0 ).rgb;
            }
            imageStore( GiDiffuseOutTexture, pixel, vec4( diffuse, 1.0 ) );
            imageStore( GiSpecularOutTexture, pixel, vec4( specular, 1.0 ) );
            imageStore( GiBackfaceOutTexture, pixel, vec4( backface, 1.0 ) );
        }
    }
}
