// DesertAsset {"Kind":"Shader","Guid":"75e19b751a1e4e9cbdb4b3c70fc2a084","Versions":{"SHDR":1},"Dependencies":[]}
Shader "RayTracedLocalShadow"
{
    // "Shadows: Ray Traced Local" (UE RayTracingShadows.cpp / RayTracingOcclusionRGS.usf for a point or spot light,
    // an inline ray query instead of a ray-generation shader): every G-buffer pixel the light reaches traces
    // SamplesPerPixel rays at the light's emitter sphere (UE SampleSphereLight over SourceRadius,
    // Common/LightSourceSampling.glslh - MegaLights' sampler), each bounded where it meets the sphere, and writes the
    // fraction that reached the emitter plus the closest occluder distance the ShadowVisibility denoiser sizes its
    // penumbra kernel with. Outside the light's radius or cone: lit (1), no occluder (-1) - the light adds nothing
    // there. SourceRadius 0 = one ray at the centre, a hard shadow.

    Compute
    {
        #define RAY_QUERY_VISIBILITY_TLAS_BINDING      0
        #define RAY_QUERY_VISIBILITY_GEOMETRY_BINDING  3
        #define RAY_QUERY_VISIBILITY_MATERIALS_BINDING 4
        #include <Common/RayQueryVisibility.glslh>
        #include <Common/ConeSampling.glslh>
        #include <Common/LightSourceSampling.glslh>
        #include <Common/ReconstructPosition.glslh>

        Uniform sampler PointClampSampler;
        Uniform(1) texture2D GBufferDepthTexture;  // device depth, reversed-Z (0 = far: no surface)
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(2) texture2D GBufferNormalTexture; // rgb = world normal, a = roughness
        #define GBufferNormalTexture sampler2D(GBufferNormalTexture, PointClampSampler)
        layout(binding = 5, rgba16f) writeonly uniform image2D ShadowMaskTexture;     // r = visibility, 1 = lit
        layout(binding = 6, rgba16f) writeonly uniform image2D ShadowOccluderTexture; // r = closest occluder, cm; -1 = none

        // RayTracedLocalShadowPush (RayTracedLocalShadow.hpp).
        PushConstant PushConstants
        {
        	mat4 InverseJitteredViewProjection;
        	vec4 LightSphere;       // xyz = position cm, w = attenuation radius cm
        	vec4 TraceParams; // x = source radius cm, y = normal bias cm, z = samples per pixel, w = frame index
        	vec4 SpotCone;        // xyz = direction out of the cone apex, w = cos outer (-2 = a point light)
        };

        LocalSize(8, 8, 1);

        void WriteUnoccluded(ivec2 pixel)
        {
            imageStore( ShadowMaskTexture, pixel, vec4( 1.0 ) );
            imageStore( ShadowOccluderTexture, pixel, vec4( -1.0 ) );
        }

        void main()
        {
            const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
            const ivec2 size  = imageSize( ShadowMaskTexture );
            if ( pixel.x >= size.x || pixel.y >= size.y )
                return;

            const float depth = texelFetch( GBufferDepthTexture, pixel, 0 ).r;
            if ( depth <= 0.0 )
            {
                WriteUnoccluded( pixel );
                return;
            }
            const vec2 uv       = ( vec2( pixel ) + 0.5 ) / vec2( size );
            const vec3 position = ReconstructWorldPosition( uv, depth, InverseJitteredViewProjection );
            const vec3 toLight  = LightSphere.xyz - position;
            const float distance = length( toLight );
            if ( distance > LightSphere.w || distance <= 0.0 )
            {
                WriteUnoccluded( pixel );
                return;
            }
            const vec3 axis = toLight / distance;
            if ( dot( -axis, normalize( SpotCone.xyz ) ) < SpotCone.w )
            {
                WriteUnoccluded( pixel );
                return;
            }
            const vec3  normal  = normalize( texelFetch( GBufferNormalTexture, pixel, 0 ).rgb );
            const float bias    = TraceParams.y;
            const uint  samples = max( uint( TraceParams.z ), 1u );
            float       visible  = 0.0;
            float       occluder = -1.0;
            for ( uint s = 0u; s < samples; ++s )
            {
                float      reach;
                const vec3 dir = SampleLightSourceSphere( axis, distance, TraceParams.x,
                                                          ConeRandom2( uvec2( pixel ), uint( TraceParams.w ), s ), reach );
                // UE r.RayTracing.NormalBias: off the surface along its normal, on the side facing the ray; the
                // ray stops short of the emitter by the same bias at both ends.
                const vec3  origin = position + normal * ( dot( normal, dir ) >= 0.0 ? 1.0 : -1.0 ) * bias;
                const float tMax   = max( reach - 2.0 * bias, 0.0 );
                float       hitT   = tMax;
                const float v      = RayQueryVisibilityHit( origin, dir, tMax, RAY_TRACING_MASK_SHADOW_CASTER, hitT );
                visible += v;
                if ( v == 0.0 )
                    occluder = occluder < 0.0 ? hitT : min( occluder, hitT );
            }
            imageStore( ShadowMaskTexture, pixel, vec4( visible / float( samples ) ) );
            imageStore( ShadowOccluderTexture, pixel, vec4( occluder ) );
        }
    }
}
