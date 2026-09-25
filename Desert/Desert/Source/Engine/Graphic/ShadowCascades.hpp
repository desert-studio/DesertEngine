#pragma once

#include <Common/Core/Units.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Desert::Graphic
{
    // Cascaded-shadow-map cascade fitting, as PURE MATH — no renderer, no GPU, no state.
    //
    // It lived inside MeshRenderer, where it could only be exercised by running the editor and looking at
    // the screen. That is how a metre-era constant survived the switch to centimetres and quietly capped
    // every shadow at 150 cm: nothing could assert on it. Out here it is a function of numbers that a test
    // can pin down (see Tests/Engine/ShadowCascades).
    //
    // EVERY distance is in WORLD UNITS, and a world unit is a centimetre (Common::Units).

    inline constexpr uint32_t kMaxShadowCascades = 4;

    // WHAT A RENDERER SPENDS ON THE SUN'S SHADOW, chosen when the renderer is CREATED and fixed for its
    // life — the cascade framebuffers are allocated once, in MeshRenderer::SetupShadowPass.
    //
    // It exists because the three numbers are one decision and were three constants. A viewport of a
    // level wants 4 cascades at 2048 over 150 m; a 512-px asset preview showing one object on a floor
    // wants none of that, and paying for it is not a nicety: four 2048 RGBA32F colour attachments plus
    // their D24S8 depth is 320 MiB PER RENDERER (ShadowAttachmentBytes below is the one place that
    // arithmetic lives), and this editor allows six live renderers at once. That is why the preview scene
    // had shadows switched off outright (a floor with no shadow under the ball), and switching them back
    // on as they stood would have been nearly 2 GiB of attachments for six open windows.
    //
    // THE THREE MOVE TOGETHER OR THE RESULT IS A BLOB. Dropping to one cascade while keeping 150 m makes
    // the single map cover the whole distance, so a 1 m object's shadow lands in about seven texels of
    // 1024 — cheaper and useless. The quantity that decides whether a shadow is usable is one texel's
    // world size, 2*Radius/ShadowMapSize, which is what Desert/Tests/Engine/ShadowCascades asserts about
    // the drawing presets below rather than asserting each number on its own. (The third preset,
    // kNoShadowQuality, is outside that relation on purpose: it draws nothing, so no texel of it has a
    // world size to be wrong about.)
    struct ShadowQuality
    {
        uint32_t CascadeCount  = kMaxShadowCascades;
        uint32_t ShadowMapSize = 2048;
        // How far from the camera shadows are computed at all — see CascadeSetup::MaxDistance.
        float MaxDistance = Common::Units::Metres( 150.0f );

        bool operator==( const ShadowQuality& ) const = default;
    };

    // A viewport of a level: what every renderer in this engine used before the budget was nameable.
    inline constexpr ShadowQuality kSceneShadowQuality{};

    // An asset preview: ONE cascade at 1024 over 10 m. Ten metres is the whole preview world — the
    // primitives are 1 m across (PrimitiveMeshFactory::kPrimitiveSize) and the floor a few metres — so
    // the single cascade is spent entirely on the subject instead of on empty distance.
    inline constexpr ShadowQuality kPreviewShadowQuality{ 1u, 1024u, Common::Units::Metres( 10.0f ) };

    // A RENDERER THAT WILL NEVER DRAW A SHADOW, and therefore holds no map to draw one into. Zero is a
    // legal cascade count everywhere the count already travels: ComputeShadowCascades returns 0,
    // RegisterShadowPass registers no pass, and SceneShadowBind writes 0 into u_ShadowParams.w so the
    // shader's own loop selects no cascade and ShadowFactor returns "lit" without sampling a map.
    //
    // WHY THIS, AND NOT `SceneSettings::EnableShadows`. The flag looks like the same decision and is a
    // different one, in the only way that matters here — its LIFETIME. It is per-scene, serialized,
    // toggleable from Scene Settings mid-session, and read afresh every frame; the cascade framebuffers
    // are allocated once, in Initialize, and live as long as the renderer. Two of the three sites that
    // want no shadows (AssetThumbnailRenderer, PhotogrammetryPanel) also set that flag AFTER
    // Scene::Init() has already run, so an allocation gated on it would have read `true` and paid in
    // full at exactly the sites the saving was for. The budget is asked at CONSTRUCTION, which is when
    // it is knowable and when the answer stops changing.
    //
    // All three numbers are zero on purpose rather than "4 cascades of size 0": the decision is "this
    // renderer spends nothing on the sun", and a resolution or a distance left standing beside a count
    // of zero is a number the next reader has to work out is dead.
    inline constexpr ShadowQuality kNoShadowQuality{ 0u, 0u, 0.0f };

    // WHAT A BUDGET COSTS IN ATTACHMENT MEMORY. Derived from the two formats MeshRenderer::SetupShadowPass
    // actually pushes — one RGBA32F colour map plus one DEPTH24STENCIL8 per cascade — because this figure
    // had two independent spellings (the renderer's log line and the test's own lambda) and a third would
    // have been written the next time somebody wanted it. 20 bytes a texel: an estimate of "about a byte
    // a texel" is wrong by a factor of four, which is the difference between 80 MB and 320 MB.
    inline constexpr uint64_t kShadowBytesPerTexel =
         Core::Formats::GetBytesPerPixel( ViewTargetFormats::kShadowColor ) +
         Core::Formats::GetBytesPerPixel( ViewTargetFormats::kShadowDepth );

    [[nodiscard]] inline constexpr uint64_t ShadowAttachmentBytes( const ShadowQuality& quality )
    {
        return static_cast<uint64_t>( quality.CascadeCount ) * quality.ShadowMapSize * quality.ShadowMapSize *
               kShadowBytesPerTexel;
    }

    // THE LIVE TOTAL ACROSS EVERY RENDERER, because the per-renderer figure alone does not say how many
    // renderers hold one. An empty editor logs the allocation line TWICE — SceneRenderer::Init runs again
    // when the project's default scene loads — and two lines saying "320 MB" were read as 640 MB held for
    // nothing. They are one renderer: the first set is released before the second is allocated. Nothing
    // in the log said so, and no arithmetic over the per-renderer line can say so, because the missing
    // fact is how many of them are ALIVE.
    //
    // RAII rather than a matching pair of add/remove calls, and held beside the framebuffers it accounts
    // for, so the count is destroyed exactly when they are. A hand-written decrement is the middle link
    // this project keeps losing properties in; there is no site here that can forget one.
    class ShadowAttachmentLease
    {
    public:
        ShadowAttachmentLease() = default;

        explicit ShadowAttachmentLease( const ShadowQuality& quality )
             : m_Bytes( ShadowAttachmentBytes( quality ) )
        {
            if ( m_Bytes != 0 )
            {
                s_LiveBytes += m_Bytes;
                ++s_LiveHolders;
            }
        }

        ShadowAttachmentLease( const ShadowAttachmentLease& )            = delete;
        ShadowAttachmentLease& operator=( const ShadowAttachmentLease& ) = delete;

        ShadowAttachmentLease( ShadowAttachmentLease&& other ) noexcept : m_Bytes( other.m_Bytes )
        {
            other.m_Bytes = 0;
        }

        ShadowAttachmentLease& operator=( ShadowAttachmentLease&& other ) noexcept
        {
            if ( this != &other )
            {
                Release();
                m_Bytes       = other.m_Bytes;
                other.m_Bytes = 0;
            }
            return *this;
        }

        ~ShadowAttachmentLease()
        {
            Release();
        }

        void Release()
        {
            if ( m_Bytes == 0 )
                return;
            s_LiveBytes -= m_Bytes;
            --s_LiveHolders;
            m_Bytes = 0;
        }

        // Every cascade set alive in this process, and how many renderers hold one. Read by the
        // renderer's own log line so a reader never has to guess the multiplier.
        [[nodiscard]] static uint64_t LiveBytes()
        {
            return s_LiveBytes;
        }
        [[nodiscard]] static uint32_t LiveHolders()
        {
            return s_LiveHolders;
        }

    private:
        uint64_t m_Bytes = 0;

        // Not atomic, and that is a statement rather than an omission: cascade framebuffers are created
        // and destroyed on the render thread only (MeshRenderer::Initialize runs inside
        // SceneRenderer::Init, which a JobSystem worker may not call — it touches the GPU).
        inline static uint64_t s_LiveBytes   = 0;
        inline static uint32_t s_LiveHolders = 0;
    };

    struct CascadeFit
    {
        glm::mat4 ViewProj      = glm::mat4( 1.0f ); // light-space matrix for this cascade
        glm::vec3 Center        = glm::vec3( 0.0f ); // world centre of the fitted sphere (texel-snapped)
        float     Radius        = 0.0f;              // world radius of the slice's bounding sphere
        float     WorldPerTexel = 0.0f;              // 2 * Radius / shadowMapSize — drives the normal-offset
        float     SplitFar      = 0.0f;              // view-space far distance this cascade covers
    };

    struct CascadeSetup
    {
        glm::mat4 CameraView       = glm::mat4( 1.0f );
        glm::mat4 CameraProjection = glm::mat4( 1.0f );
        float     CameraNear       = Common::Units::Cm( 10.0f );
        float     CameraFar        = Common::Units::Metres( 1000.0f );
        glm::vec3 LightDirection   = glm::vec3( 0.0f, -1.0f, 0.0f ); // direction the light TRAVELS
        // How far from the camera shadows are computed at all. Anything past this is simply unshadowed,
        // so it is the single number that decides whether a scene "has shadows".
        float    MaxDistance   = Common::Units::Metres( 150.0f );
        float    SplitLambda   = 0.6f; // 0 = uniform splits, 1 = logarithmic
        uint32_t CascadeCount  = kMaxShadowCascades;
        uint32_t ShadowMapSize = 2048;
    };

    // Copies the budget into the fit's inputs. ONE assignment site for all three, because the failure
    // this whole file was extracted for is a middle link dropping one of them: a renderer that allocated
    // one cascade and then fitted four would write three matrices no map exists for, and the shader would
    // sample a cascade that was never rendered — a correct-looking frame with the subject unshadowed.
    inline void ApplyShadowQuality( CascadeSetup& setup, const ShadowQuality& quality )
    {
        setup.CascadeCount  = quality.CascadeCount;
        setup.ShadowMapSize = quality.ShadowMapSize;
        setup.MaxDistance   = quality.MaxDistance;
    }

    // Fills @p out with CascadeCount fitted cascades. Returns how many were written (0 when the setup is
    // degenerate: no light direction, or a far plane behind the near one).
    inline uint32_t ComputeShadowCascades( const CascadeSetup& setup, CascadeFit* out )
    {
        const uint32_t count = std::min( setup.CascadeCount, kMaxShadowCascades );
        if ( count == 0 || !out )
            return 0;
        if ( glm::length( setup.LightDirection ) < 1e-4f )
            return 0;

        const glm::vec3 lightDir  = glm::normalize( setup.LightDirection );
        const float     camNear   = setup.CameraNear;
        const float     shadowFar = glm::min( setup.CameraFar, setup.MaxDistance );
        if ( shadowFar <= camNear )
            return 0;

        const float splitRange = shadowFar - camNear;
        const float ratio      = shadowFar / glm::max( camNear, 1e-4f );

        // Practical split scheme: blend uniform and logarithmic distributions (lambda).
        float splitFar[kMaxShadowCascades];
        for ( uint32_t i = 0; i < count; ++i )
        {
            const float p   = static_cast<float>( i + 1 ) / static_cast<float>( count );
            const float log = camNear * std::pow( ratio, p );
            const float uni = camNear + splitRange * p;
            splitFar[i]     = glm::mix( uni, log, setup.SplitLambda );
        }

        // Frustum corners give the eye + view axis; the slice bounding spheres are then computed
        // analytically from scalars, which is what keeps the radius bit-stable as the camera turns
        // (a centroid + max-corner-distance is analytically rotation-invariant but accumulates FP noise, and
        // the quantized radius then flip-flops and the texel snap stops hiding the crawl).
        //
        // THE NDC z VALUES BELONG TO THE CAMERA'S CONVENTION, NOT TO THIS FUNCTION'S. The camera is
        // REVERSED-Z (Core/Projection.hpp): 1 on the near plane, 0 on the far one. Reading them the GL way
        // (-1 near, +1 far) does not merely misplace the rings — it SWAPS them, `viewFwd` comes out
        // pointing behind the camera, and every cascade is fitted to the space at the observer's back.
        // The cascades' own projection is a separate decision, made where it is built below.
        const glm::mat4 invVP = glm::inverse( setup.CameraProjection * setup.CameraView );
        glm::vec3       nearCorners[4];
        glm::vec3       farCorners[4];
        int             ci = 0;
        for ( int x = 0; x < 2; ++x )
        {
            for ( int y = 0; y < 2; ++y )
            {
                const glm::vec4 nc = invVP * glm::vec4( 2.0f * x - 1.0f, 2.0f * y - 1.0f, 1.0f, 1.0f );
                const glm::vec4 fc = invVP * glm::vec4( 2.0f * x - 1.0f, 2.0f * y - 1.0f, 0.0f, 1.0f );
                nearCorners[ci]    = glm::vec3( nc ) / nc.w;
                farCorners[ci]     = glm::vec3( fc ) / fc.w;
                ++ci;
            }
        }

        const glm::vec3 nearRingCenter =
             0.25f * ( nearCorners[0] + nearCorners[1] + nearCorners[2] + nearCorners[3] );
        const glm::vec3 farRingCenter = 0.25f * ( farCorners[0] + farCorners[1] + farCorners[2] + farCorners[3] );
        const glm::vec3 viewFwd       = glm::normalize( farRingCenter - nearRingCenter );
        const glm::vec3 eye           = nearRingCenter - viewFwd * camNear; // frustum apex

        // k = the frustum's angular half-slope, taken from the PROJECTION matrix (rotation-independent).
        const float tanHalfY = 1.0f / glm::max( std::abs( setup.CameraProjection[1][1] ), 1e-6f );
        const float aspect   = std::abs( setup.CameraProjection[1][1] / setup.CameraProjection[0][0] );
        const float kSlope   = tanHalfY * std::sqrt( 1.0f + aspect * aspect );
        const float k2       = kSlope * kSlope;

        float lastFar = camNear;
        for ( uint32_t c = 0; c < count; ++c )
        {
            const float zNear = lastFar;
            const float zFar  = splitFar[c];

            float sphereZ = 0.0f;
            float radius  = 0.0f;
            if ( k2 * ( zFar + zNear ) >= ( zFar - zNear ) )
            {
                // The near ring is already inside the far ring's sphere.
                sphereZ = zFar;
                radius  = kSlope * zFar;
            }
            else
            {
                sphereZ        = 0.5f * ( zFar + zNear ) * ( 1.0f + k2 );
                const float dz = sphereZ - zFar;
                radius         = std::sqrt( dz * dz + k2 * zFar * zFar );
            }

            const glm::vec3 center = eye + viewFwd * sphereZ;
            radius                 = std::ceil( radius * 16.0f ) / 16.0f; // quantize a bit for stability

            const glm::vec3 up = glm::abs( lightDir.y ) > 0.99f ? glm::vec3( 0, 0, 1 ) : glm::vec3( 0, 1, 0 );

            // Texel-snap: the cascade centre may only move in WHOLE shadow-map texels along the light's
            // axes, so the sampling grid stays world-locked while the camera moves.
            const float     worldPerTexel = ( 2.0f * radius ) / static_cast<float>( setup.ShadowMapSize );
            const glm::mat4 lightBasis    = glm::lookAt( -lightDir, glm::vec3( 0.0f ), up );
            glm::vec3       centerLS      = glm::vec3( lightBasis * glm::vec4( center, 1.0f ) );
            centerLS.x                    = std::floor( centerLS.x / worldPerTexel ) * worldPerTexel;
            centerLS.y                    = std::floor( centerLS.y / worldPerTexel ) * worldPerTexel;
            const glm::vec3 snapped       = glm::vec3( glm::inverse( lightBasis ) * glm::vec4( centerLS, 1.0f ) );

            // The light eye is pushed back by 2*radius so casters between it and the slice still cast.
            const glm::mat4 view = glm::lookAt( snapped - lightDir * ( radius * 2.0f ), snapped, up );
            // DELIBERATELY STANDARD-Z (0 at the near plane, 1 at the far one) while the camera is
            // reversed-Z. An orthographic projection's depth is LINEAR in light-space distance, so the
            // float exponent is already spread evenly across the slice and reversing it would gain
            // precisely nothing — while costing an inverted compare in the seven shaders that sample
            // these maps and a re-tuned shadow bias. The pass that consumes this matrix says so too
            // (MeshRenderer::SetupShadowPass) and clears its depth to 1 rather than the engine's 0.
            const glm::mat4 proj =
                 glm::orthoRH_ZO( -radius, radius, -radius, radius, Common::Units::Cm( 10.0f ), radius * 4.0f );

            out[c].ViewProj      = proj * view;
            out[c].Center        = snapped;
            out[c].Radius        = radius;
            out[c].WorldPerTexel = worldPerTexel;
            out[c].SplitFar      = zFar;

            lastFar = zFar;
        }
        return count;
    }
} // namespace Desert::Graphic
