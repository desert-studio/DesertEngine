// DesertAsset {"Kind":"Shader","Guid":"5d1c7e0a9b3f4e62a8d4c1f07e9b2a63","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterUnderwater"
{
    Fragment
    {
        // THE UNDERWATER VOLUME (WATER-UNDERWATER; UE Water: the body's underwater post process, its material
        // reading the body's own scattering and absorption). A full-screen triangle drawn by
        // Graphic::System::WaterRenderer ("Deferred: Underwater") over the scene target BEFORE the water surface,
        // while the camera's near plane reaches under a body's surface (WaterRenderer::PrepareUnderwater):
        //   - THE MASK, per pixel: the pixel's near-plane point under the surface at the camera - the waterline
        //     crosses the screen where the near plane crosses the water, so half-submerged views split cleanly
        //     and a pixel above the line is left as it is (discard: the target is loaded);
        //   - THE VOLUME over the eye ray to the opaque depth (or to where the ray leaves through the still
        //     surface): Beer-Lambert on what is behind, the medium's single scattering of the sun and sky in
        //     front (Water/WaterUnderwater.glslh), and the scene behind lit only by what reached its depth.
        // It reads the opaque scene's copy (the target as it is before the water: no feedback) and the G-buffer
        // depth; the surface is drawn over it, its underside fogged from the eye by the same medium.

        In(0) vec2 v_TexCoord;
        Out(0) vec4 o_Color;

        Uniform(0) sampler PointClampSampler;
        Uniform(1) sampler LinearClampSampler;
        Uniform(2) texture2D SceneColorTexture;
        #define SceneColorTexture sampler2D(SceneColorTexture, PointClampSampler)
        Uniform(3) texture2D SceneDepthTexture;
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)
        Uniform(4) textureCube EnvironmentIrradianceTexture;
        #define EnvironmentIrradianceTexture samplerCube(EnvironmentIrradianceTexture, LinearClampSampler)

        // A uniform block, not a push block: 176 bytes is past the 128 Vulkan guarantees (WaterRenderer::UploadUnderwater).
        Uniform(5) UnderwaterUB
        {
            mat4 InvViewProjection; // the view's jittered inverse (the depth was rasterised jittered)
            vec4 CameraSurface;     // xyz = the camera (cm), w = the surface height at the camera (cm)
            vec4 Scattering;        // rgb = scattering 1/m, a = phase g
            vec4 Absorption;        // rgb = absorption 1/m, a = the view's pre-exposure
            vec4 ToSun;             // xyz = toward the sun
            vec4 SunRadiance;       // rgb = colour * intensity (not pre-exposed)
            vec4 SkyYawCosSin;      // xy = the sky look's (cos yaw, sin yaw)
            vec4 SkyGain;           // rgb = the sky look's tint * intensity
        };

        #include <Common/SkyLook.glslh>
        #include <Water/WaterUnderwater.glslh>

        vec3 WorldAt( vec2 uv, float deviceDepth )
        {
            const vec4 w = InvViewProjection * vec4( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, deviceDepth, 1.0 );
            return w.xyz / w.w;
        }

        void main()
        {
            const vec3  camera   = CameraSurface.xyz;
            const float surfaceY = CameraSurface.w;
            // Reversed-Z: the near plane is device depth 1.
            const vec3 nearPoint = WorldAt( v_TexCoord, 1.0 );
            if ( nearPoint.y >= surfaceY )
                discard; // this pixel's ray starts in the air: the surface (drawn next) is what it meets

            const float dev    = texture( SceneDepthTexture, v_TexCoord ).r;
            const vec3  far    = WorldAt( v_TexCoord, dev <= 0.0 ? 1e-7 : dev );
            const vec3  rayDir = normalize( far - nearPoint );
            float       rayLen = dev <= 0.0 ? 1e7 : distance( nearPoint, far );
            // The ray leaves the water through the still surface (the surface drawn next covers the waves).
            if ( rayDir.y > 1e-4 )
                rayLen = min( rayLen, ( surfaceY - nearPoint.y ) / rayDir.y );

            const vec3 sigmaS = Scattering.rgb;
            const vec3 sigmaT = max( Absorption.rgb + sigmaS, vec3( 1e-5 ) );
            const vec3 skyIrr = ApplySkyGain( texture( EnvironmentIrradianceTexture,
                                                       SkyLookDirection( vec3( 0.0, 1.0, 0.0 ), SkyYawCosSin.xy ) ).rgb,
                                              SkyGain.rgb );
            vec3       inscatter;
            const vec3 trans = UnderwaterSegment( sigmaS, sigmaT, Scattering.a, rayDir, surfaceY - nearPoint.y, rayLen,
                                                  normalize( ToSun.xyz ), SunRadiance.rgb, skyIrr, inscatter );

            // The opaque point behind, when it is under the water itself, was lit by the full sun and sky (the
            // deferred lighting knows no water): only what reached its depth lights it.
            vec3 behind = texture( SceneColorTexture, v_TexCoord ).rgb; // pre-exposed
            if ( dev > 0.0 && far.y < surfaceY )
                behind *= exp( -sigmaT * ( ( surfaceY - far.y ) * 0.01 ) );

            o_Color = vec4( behind * trans + inscatter * Absorption.a, 1.0 );
        }
    }

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord  = FullscreenTriangleUV();
            gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }
}
