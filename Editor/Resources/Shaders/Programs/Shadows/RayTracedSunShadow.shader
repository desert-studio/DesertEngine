// DesertAsset {"Kind":"Shader","Guid":"4be1c07a9d2f4e6b8a35f0c19e7d2a61","Versions":{"SHDR":1},"Dependencies":[]}
Shader "RayTracedSunShadow"
{
    // "Shadows: Ray Traced Sun" (UE RayTracingShadows.cpp / RayTracingOcclusionRGS.usf, an inline ray query instead
    // of a ray-generation shader): every G-buffer pixel traces SamplesPerPixel rays toward the sun, each inside the
    // sun's cone (UE GetLightSourceDirection over the light's angular diameter SourceAngle), and writes the fraction
    // that reached the sky into the screen shadow mask (UE FScreenShadowMaskTexture) plus the closest occluder
    // distance the ShadowVisibility denoiser sizes its penumbra kernel with. Angle 0 = one direction, a hard shadow.

    Compute
    {
        #define RAY_QUERY_VISIBILITY_TLAS_BINDING      0
        #define RAY_QUERY_VISIBILITY_GEOMETRY_BINDING  3
        #define RAY_QUERY_VISIBILITY_MATERIALS_BINDING 4
        #include <Common/RayQueryVisibility.glslh>
        #include <Common/ConeSampling.glslh>
        #include <Common/ReconstructPosition.glslh>

        // Shared sampler states (UE shared samplers): each input below is a separate texture read through one of
        // these, bound by the pass by name and numbered by the program build (ShaderMapBuild.cpp
        // AssignSharedSamplerBindings).
        Uniform sampler u_PointClampSampler;
        Uniform(1) texture2D u_GBufferDepth;  // device depth, reversed-Z (0 = far: no surface)
        #define u_GBufferDepth sampler2D(u_GBufferDepth, u_PointClampSampler)
        Uniform(2) texture2D u_GBufferNormal; // rgb = world normal, a = roughness
        #define u_GBufferNormal sampler2D(u_GBufferNormal, u_PointClampSampler)
        layout(binding = 5, rgba16f) writeonly uniform image2D u_ShadowMask;       // r = visibility, 1 = lit
        layout(binding = 6, rgba16f) writeonly uniform image2D u_ShadowOccluder; // r = closest occluder, cm; -1 = none

        // RayTracedSunShadowPush (RayTracedSunShadow.hpp): the view's inverse + RayTracedSunShadowParams.
        PushConstant PushConstants
        {
        	mat4 u_InvJitteredViewProjection;
        	vec4 u_DirectionToSun; // xyz = toward the sun, w = SourceAngle in radians
        	vec4 u_TraceParams;    // x = normal bias cm, y = samples per pixel, z = max distance cm, w = frame index
        };

        LocalSize(8, 8, 1);

        void main()
        {
            const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
            const ivec2 size  = imageSize( u_ShadowMask );
            if ( pixel.x >= size.x || pixel.y >= size.y )
                return;

            const float depth = texelFetch( u_GBufferDepth, pixel, 0 ).r;
            if ( depth <= 0.0 )
            {
                imageStore( u_ShadowMask, pixel, vec4( 1.0 ) );
                imageStore( u_ShadowOccluder, pixel, vec4( -1.0 ) );
                return;
            }
            const vec2 uv       = ( vec2( pixel ) + 0.5 ) / vec2( size );
            // Common/ReconstructPosition.glslh: (0,0) is the top-left pixel and NDC +y is up.
            const vec3 position = ReconstructWorldPosition( uv, depth, u_InvJitteredViewProjection );
            const vec3 normal   = normalize( texelFetch( u_GBufferNormal, pixel, 0 ).rgb );
            const vec3 toSun    = normalize( u_DirectionToSun.xyz );
            // UE r.RayTracing.NormalBias: off the surface along its normal, on the side facing the light.
            const vec3 origin = position + normal * ( dot( normal, toSun ) >= 0.0 ? 1.0 : -1.0 ) * u_TraceParams.x;

            const uint  samples   = max( uint( u_TraceParams.y ), 1u );
            const float halfAngle = 0.5 * u_DirectionToSun.w;
            float       visible   = 0.0;
            float       occluder  = -1.0;
            for ( uint s = 0u; s < samples; ++s )
            {
                vec3 dir = toSun;
                if ( halfAngle > 0.0 )
                    dir = ConeDirection( toSun, halfAngle, ConeRandom2( uvec2( pixel ), uint( u_TraceParams.w ), s ) );
                float       hitT = u_TraceParams.z;
                const float v =
                     RayQueryVisibilityHit( origin, dir, u_TraceParams.z, RAY_TRACING_MASK_SHADOW_CASTER, hitT );
                visible += v;
                if ( v == 0.0 )
                    occluder = occluder < 0.0 ? hitT : min( occluder, hitT );
            }
            imageStore( u_ShadowMask, pixel, vec4( visible / float( samples ) ) );
            imageStore( u_ShadowOccluder, pixel, vec4( occluder ) );
        }
    }
}
