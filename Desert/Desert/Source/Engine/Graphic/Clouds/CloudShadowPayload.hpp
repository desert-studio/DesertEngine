#pragma once

#include <Common/Core/Math/Rounding.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    // Forward-declared rather than included: CloudShadowInput below only borrows the map, and this header
    // is compiled as-is by Desert/Tests/Engine/CloudShadow, which has no renderer to link against.
    class Image2D;

    /**
     * The C++ half of the cloud shadow map: the numbers that size it, the projection that aims it, and
     * the two blocks that carry it to the GPU.
     *
     * WHAT THE MAP IS, in one sentence, because the rest of this file only makes sense after it: an
     * orthographic map aimed down the sun's own direction, whose every texel holds
     * `(frontDepthKm, meanExtinctionPerKm, maxOpticalDepth)` — the triple that reconstructs a correct
     * transmittance for a receiver at ANY depth along that texel's ray with one fetch. The encoding, the
     * reconstruction and the march that fills it are all in Editor/Resources/Shaders/Common/
     * CloudShadowMap.glslh, which is compiled both as GLSL (by the producer and by the deferred lighting
     * pass) and as C++ (by Desert/Tests/Engine/CloudShadow).
     *
     * THE CONSTANTS BELOW ARE MIRRORS. Their statement of record is that header — the shaders read the
     * `#define`s, this file restates them for the renderer and for the C++ projection maths, and
     * Desert/Tests/Engine/CloudShadow asserts the two sides are equal by compiling both. Same arrangement
     * as CloudGpuPayload against Common/CloudParams.glslh, and for the same reason: a mirror that nobody
     * checks is a mirror that drifts, and the symptom of drift here is a shadow in the wrong place, which
     * reads as a bias problem rather than as a constant.
     */

    // Texels per side AT THE REFERENCE TIER. Unreal's own default (`512 * CloudShadowMapResolutionScale`,
    // VolumetricCloudRendering.cpp:429), and the number the extent is derived FROM rather than fitted to.
    inline constexpr uint32_t kCloudShadowMapResolution = 512u;

    // Half the side of the world square the map covers at the reference tier, kilometres. Follows from the
    // resolution and from the finest cloud chord the view march can find (0.125 km at the component's
    // default 256 steps): 512 x 0.125 km = 64 km across, a radius of 32, rounded down to 30 so the texel
    // sits just inside the chord at 0.1172 km. The derivation and its consequences are in
    // CloudShadowMap.glslh.
    inline constexpr float kCloudShadowExtentKm = 30.0f;

    // The world grid the map's centre snaps to at the reference tier, kilometres. Unreal's
    // r.VolumetricCloud.ShadowMap.SnapLength, same twenty kilometres, and the whole reason the shadow does
    // not boil under a moving camera.
    inline constexpr float kCloudShadowSnapKm = 20.0f;

    // The WORLD width of the band at the border over which the shadow fades to fully lit, kilometres, so
    // the map's edge is a gradient rather than a straight line of light across a terrain.
    //
    // KILOMETRES AND NOT UV, and the change is what makes the fade survive a quality tier. The number the
    // shader wants is a UV width and the number that MEANS something is a world width: the fade has to be
    // wide enough to read as a gradient and to span enough texels not to band, and both of those are world
    // quantities. At the reference tier 1.2 km over a 60 km side IS the 0.02 this constant used to be
    // written as, to the digit — so nothing about the shipped picture moves — but on a tier whose map
    // covers half as much world, a fixed 0.02 would have halved the band to five texels and put a visible
    // ring where a gradient belongs. Desert/Tests/Engine/CloudShadow asserts the band spans at least ten
    // texels ON EVERY TIER, which is the assertion this expression exists to keep true.
    inline constexpr float kCloudShadowBorderFadeKm = 1.2f;

    /// The map's texels per side at a tier's scale. Floored at 64 so a scale small enough to round to
    /// nothing still produces a dispatchable image rather than a zero-sized one.
    inline uint32_t CloudShadowResolutionForScale( float scale )
    {
        const float scaled = static_cast<float>( kCloudShadowMapResolution ) * std::max( scale, 0.0f );
        return std::max( 64u, static_cast<uint32_t>( Common::Math::RoundToNearest( scaled ) ) );
    }

    /// Half the side of the covered square at a tier's scale, kilometres.
    inline float CloudShadowExtentKmForScale( float scale )
    {
        return kCloudShadowExtentKm * std::max( scale, 1e-3f );
    }

    /// The world grid the centre snaps to at a tier's scale, kilometres. It scales WITH the extent because
    /// the coverage guaranteed around the camera is `extent - snap/2`; a snap left at its reference value
    /// on a smaller map would eat that guarantee from the inside.
    inline float CloudShadowSnapKmForScale( float scale )
    {
        return kCloudShadowSnapKm * std::max( scale, 1e-3f );
    }

    /// The border fade as the shader wants it — a UV width — derived from the world width above and the
    /// map's actual side. One expression, so the two cannot drift.
    inline float CloudShadowBorderFadeUv( float extentKm )
    {
        return kCloudShadowBorderFadeKm / std::max( 2.0f * extentKm, 1e-3f );
    }

    // Samples a texel's ray takes with the sun high, and how much that count is allowed to grow as the sun
    // reaches the horizon and every ray's path through the shell lengthens. Unreal's
    // RaySampleMaxCount / RaySampleHorizonMultiplier pair.
    inline constexpr float kCloudShadowBaseSamples       = 32.0f;
    inline constexpr float kCloudShadowHorizonMultiplier = 2.0f;

    // The bindings of the shadow map compute pass. As everywhere else in this subsystem, SetOutput /
    // SetStorageBuffer / SetInput take these verbatim and never consult reflection, so each must equal the
    // number written in Programs/Clouds/CloudShadowMap.shader.
    //
    // THE PARAMS, NOISE AND PROFILE BINDINGS ARE THE MARCH'S OWN NUMBERS, deliberately: the two passes
    // sample the same field through the same three resources, and giving them different slot numbers would
    // be two vocabularies for one thing. `kCloudParamsBinding` in particular is fixed by
    // CLOUD_PARAMS_BINDING inside Common/CloudParams.glslh, which both shaders include.
    inline constexpr uint32_t kCloudShadowOutputBinding  = 0; // the RGBA32F triple this pass writes
    inline constexpr uint32_t kCloudShadowParamsBinding  = kCloudParamsBinding;
    inline constexpr uint32_t kCloudShadowNoiseBinding     = kCloudNoiseBinding;
    inline constexpr uint32_t kCloudShadowModellingBinding = kCloudModellingBinding;

    /// All FOUR of the march's noise slots, because a cloud shades the ground with the edge it actually
    /// has: a cirrus eroded by the fine volume for the eye and by the default one for the shadow map would
    /// be two different clouds in one frame. The same array, aliased rather than restated — a second list
    /// of four numbers is the two-statements-of-one-fact defect with four chances to make it.
    inline constexpr const uint32_t ( &kCloudShadowNoiseBindings )[kCloudSpeciesSlots] = kCloudNoiseBindings;

    /**
     * The shadow map's projection for this frame.
     *
     * WorldToMap takes a world position (centimetres) to the map's clip space: xy in [-1, 1] across the
     * covered square, z in [0, 1] from the near plane to the far one. MapToWorld is its inverse and is
     * what the producer reconstructs each texel's ray origin with.
     *
     * PLAIN, NOT REVERSED-Z. The engine's depth buffers are reversed because that is where float precision
     * is won; this map has no depth buffer — its z is a number the shader multiplies by FarDepthKm — so
     * the reference's `FReversedZOrthoMatrix` and its `1 - z` would have been an inversion carried through
     * two shaders for nothing.
     */
    struct CloudShadowMapView
    {
        glm::mat4 WorldToMap{ 1.0f };
        glm::mat4 MapToWorld{ 1.0f };
        // The direction the light TRAVELS (away from the sun), normalized — the direction each texel's ray
        // is marched along.
        glm::vec3 LightDirection{ 0.0f, -1.0f, 0.0f };
        // The kilometres the map's clip z spans. The one number besides the matrix that a consumer needs.
        float FarDepthKm = 0.0f;
        // Samples this frame's rays take. Rises toward the horizon, where each ray has much further to
        // travel through the shell — Unreal's RaySampleHorizonMultiplier.
        float SampleCount = 0.0f;
        // The UV width of the border fade — a property of THIS frame's map rather than a constant, since
        // the quality tier scales the extent the fixed world width is a fraction of. It travels on the
        // view for the same reason FarDepthKm does: the consumer is on the far side of the render graph
        // and must be TOLD what it was handed rather than re-deriving it from a constant that no longer
        // describes this frame.
        //
        // The resolution is NOT here, and the omission is deliberate: the producer already has it (it is
        // what it dispatches over) and nothing downstream reads it, so a copy on the view would be a
        // field that exists to look complete.
        float BorderFadeUv = 0.0f;
    };

    /**
     * The producer's push constant: everything that changes with the sun and the camera rather than with
     * the weather.
     *
     * Ninety-six bytes, inside the 128 Vulkan guarantees.
     *
     * THE FAR DEPTH IS HERE NOW, and it used to be derived in the shader from the same
     * `CLOUD_SHADOWMAP_EXTENT_KM` the shader compiles. That worked while the extent was one constant for
     * every frame ever rendered; it stops working the moment a quality tier scales it, and the failure
     * mode is the quiet kind — the producer would encode depths against a 120 km far plane while the
     * projection it was handed spans 60, so every front depth in the map would be right by construction
     * and wrong by a factor of two the moment it was read. The consumer was already handed this number
     * explicitly (see CloudShadowUniforms) for exactly this reason; now both sides are.
     */
    struct CloudShadowPush
    {
        glm::mat4 MapToWorld;
        // xyz = the direction the light travels, normalized; w = how many samples this ray takes.
        glm::vec4 Trace;
        // x = the kilometres the map's clip z spans. The remaining three are unwritten: a push constant is
        // laid out in vec4s and this is the only scalar the producer still needs.
        glm::vec4 Depth;
    };

    static_assert( offsetof( CloudShadowPush, MapToWorld ) == 0 );
    static_assert( offsetof( CloudShadowPush, Trace ) == 64 );
    static_assert( offsetof( CloudShadowPush, Depth ) == 80 );
    static_assert( sizeof( CloudShadowPush ) == 96 );

    /**
     * The consumer's uniform block — the CloudShadowUB of Programs/Deferred/DeferredLighting.shader,
     * member for member.
     *
     * EVERY SLOT OF `Params` IS A PROPERTY OF THE MAP, not a constant the reader could look up. That is
     * the reason the far depth and the border fade travel here rather than being re-derived from the
     * `#define`s the deferred pass also compiles: the consumer must be told what it was handed, so that
     * the day the extent becomes authorable — or a second, coarser map is added beside this one — nothing
     * downstream has to change. The strength is here for the opposite reason: it is the artist's, it is
     * applied at read time rather than baked in, and one multiply in one place is the whole of it.
     */
    struct CloudShadowUniforms
    {
        glm::mat4 WorldToMap{ 1.0f };
        // x = the kilometres the clip z spans, y = 1 when the map is bound and must be read, z = the UV
        // width of the border fade, w = the artist's shadow strength.
        glm::vec4 Params{ 0.0f };
    };

    static_assert( offsetof( CloudShadowUniforms, WorldToMap ) == 0 );
    static_assert( offsetof( CloudShadowUniforms, Params ) == 64 );
    static_assert( sizeof( CloudShadowUniforms ) == 80 );

    /**
     * WHAT A RENDERER GATHERED ABOUT THIS FRAME'S MAP, before any material has been told about it — the
     * single payload every consumer of the cloud shadow receives.
     *
     * It lives HERE, beside the uniform block it packs into, and not in a material header, because there
     * is no longer one consumer. Until Р21 the only shader in the tree that read the map was the deferred
     * composite, so this struct sat inside MaterialDeferredLighting.hpp and the forward mesh shaders, the
     * glass pass and the terrain stood in full sun under a deck that shaded the ground beside them. Now
     * SceneRenderer gathers ONE of these per frame (SceneRenderer::GetCloudShadowInput) and the deferred
     * material, the PBR materials and the terrain material are all handed the same one.
     *
     * `Map` null, or `Enabled` false, is the ordinary state: no cloud component, clouds off, casting off,
     * strength zero, or a renderer whose scene has no sky at all. Consumers then leave the sampler on its
     * dummy image and the shader is told not to read it.
     */
    struct CloudShadowInput
    {
        Image2D*  Map = nullptr;
        glm::mat4 WorldToMap{ 1.0f };
        float     FarDepthKm = 0.0f;
        float     Strength   = 0.0f;
        // A PROPERTY OF THE MAP THAT WAS BUILT, not a constant, since the quality tier scales the extent
        // the fade's fixed world width is a fraction of. Zero is the right default for the same reason
        // Strength's is: the fields only mean anything once Enabled is true.
        float BorderFadeUv = 0.0f;
        bool  Enabled      = false;

        /// Whether there is a map to read at all. The ONE predicate — a consumer that spelled it out for
        /// itself would be free to spell it differently, and the symptom of that is one render path
        /// shading with a shadow the other has switched off.
        bool IsLive() const
        {
            return Enabled && Map != nullptr && Strength > 0.0f;
        }
    };

    /**
     * The gathered payload as the shader's uniform block — the ONE place `CloudShadowUniforms` is ever
     * filled, and the reason it is a function rather than four lines at each consumer.
     *
     * PURE: no GPU, no material, no globals, so Desert/Tests/Engine/CloudShadow drives it directly. The
     * `live` flag is what `Params.y` means to Common/CloudShadowReceiver.glslh, and it is computed from
     * the input rather than trusted from it, so a caller that sets `Enabled` with no map bound gets a
     * shader that does not fetch instead of a fetch from a dummy image.
     */
    inline CloudShadowUniforms CloudShadowPackUniforms( const CloudShadowInput& input )
    {
        const bool live = input.IsLive();

        CloudShadowUniforms data;
        data.WorldToMap = live ? input.WorldToMap : glm::mat4( 1.0f );
        data.Params     = glm::vec4( input.FarDepthKm, live ? 1.0f : 0.0f, input.BorderFadeUv, input.Strength );
        return data;
    }

    /// One world component of the map's centre, snapped to the coarse grid. The C++ mirror of
    /// CloudShadowSnapToGrid in Common/CloudShadowMap.glslh; the test compiles both and asserts they agree
    /// over a sweep, because they are one function written twice.
    inline float CloudShadowSnapToGrid( float value, float snapWorld )
    {
        const float snap = std::max( snapWorld, 1e-4f );
        return std::floor( ( value + 0.5f * snap ) / snap ) * snap;
    }

    /// One clip component of a fixed world point, snapped to the map's pixel lattice. Mirror of
    /// CloudShadowSnapToPixelGrid.
    inline float CloudShadowSnapToPixelGrid( float clipComponent, float resolution )
    {
        const float halfResolution = std::max( resolution, 1.0f ) * 0.5f;
        return std::floor( clipComponent * halfResolution + 0.5f ) / halfResolution;
    }

    /// How many samples a texel's ray takes at this sun elevation. Mirror of CloudShadowSampleCount;
    /// @p sunUpDot is the cosine between the planet's up under the camera and the direction TOWARD the
    /// sun, i.e. the sine of the sun's elevation.
    inline float CloudShadowSampleCount( float sunUpDot, float baseSamples, float horizonMultiplier )
    {
        const float horizon = std::clamp( 0.2f / std::max( std::abs( sunUpDot ), 1e-4f ), 0.0f, 1.0f );
        const float extra   = std::max( horizonMultiplier - 1.0f, 0.0f );
        return baseSamples + extra * baseSamples * horizon;
    }

    /**
     * Builds this frame's projection. PURE — no GPU, no globals, no clock — so
     * Desert/Tests/Engine/CloudShadow drives it directly, which is the only way the two snaps can be
     * tested at all: their whole purpose is a property of a SEQUENCE of frames, and no single frame shows
     * it.
     *
     * THE TWO SNAPS, IN ORDER, AND WHY BOTH:
     *
     *   1. The centre — the point on the planet surface under the camera — is floored onto a
     *      @p snapKm world grid. Over any camera motion shorter than the grid the projection is then
     *      BIT FOR BIT the frame before's, so every piece of world stays in the texel it was in and the
     *      shadow is nailed to the ground. Without it the bilinear fetch under a static piece of terrain
     *      slides a fraction of a texel per frame and the shadow boils — a defect that a still frame
     *      cannot show, because a still frame has exactly one projection.
     *
     *   2. The world origin is then snapped onto the map's own pixel lattice IN CLIP SPACE. The first snap
     *      freezes where the map is; it does not freeze which way it points, and the basis rotates with
     *      the sun. This is the sub-texel residual, and it costs one matrix multiply per frame.
     *
     * Both are Unreal's (VolumetricCloudRendering.cpp:1842-1845 and :1854-1880) and the second is the one
     * that is easy to leave out, because everything looks correct without it.
     *
     * @param cameraWorld    the camera position, world units (centimetres).
     * @param sunDirection   the direction TOWARD the sun, as AtmosphereEnv publishes it.
     * @param planetRadiusKm the layer's planet radius, kilometres — the same one the march curves around.
     * @param extentKm       half the side of the covered square, kilometres.
     * @param snapKm         the coarse world grid the centre is floored onto, kilometres.
     * @param resolution     texels per side.
     */
    inline CloudShadowMapView CloudBuildShadowMapView( const glm::vec3& cameraWorld, const glm::vec3& sunDirection,
                                                       float planetRadiusKm, float extentKm, float snapKm,
                                                       float resolution )
    {
        CloudShadowMapView view;

        const float extentWorld        = std::max( extentKm, 0.001f ) * kCloudWorldUnitsPerKm;
        const float snapWorld          = std::max( snapKm, 0.001f ) * kCloudWorldUnitsPerKm;
        const float lightDistanceWorld = 2.0f * extentWorld;
        const float farPlaneWorld      = 2.0f * lightDistanceWorld;

        // A zero or degenerate sun direction is answered with straight down rather than with a NaN
        // matrix: AtmosphereEnv publishes a normalized vector, but a packer that depends on its caller
        // having checked something will one day be called by someone who did not.
        const float     sunLength   = glm::length( sunDirection );
        const glm::vec3 toSun       = sunLength > 1e-6f ? sunDirection / sunLength : glm::vec3( 0.0f, 1.0f, 0.0f );
        const glm::vec3 lightTravel = -toSun;

        // THE PLANET CENTRE IS BELOW THE WORLD ORIGIN BY ONE RADIUS. That is the march's own convention
        // (CloudRaymarch.shader: `originKm = vec3(camera.x, camera.y + planetRadiusKm, camera.z)`), so the
        // ground under the world origin is the surface of the sphere, and this is where the two have to
        // agree — a map centred on a different sphere than the one the march intersects is a shadow
        // displaced by the curvature, growing with distance from the origin.
        const glm::vec3 planetCentre( 0.0f, -std::max( planetRadiusKm, 1.0f ) * kCloudWorldUnitsPerKm, 0.0f );

        const glm::vec3 toCamera    = cameraWorld - planetCentre;
        const float     toCameraLen = glm::length( toCamera );
        const glm::vec3 planetUp    = toCameraLen > 1e-3f ? toCamera / toCameraLen : glm::vec3( 0.0f, 1.0f, 0.0f );

        glm::vec3 anchor = planetCentre + planetUp * ( std::max( planetRadiusKm, 1.0f ) * kCloudWorldUnitsPerKm );
        anchor.x         = CloudShadowSnapToGrid( anchor.x, snapWorld );
        anchor.y         = CloudShadowSnapToGrid( anchor.y, snapWorld );
        anchor.z         = CloudShadowSnapToGrid( anchor.z, snapWorld );

        // lookAt is degenerate when the view direction and the up vector are parallel, which is exactly
        // the common case here — a sun overhead travels straight down. Unreal switches to a horizontal
        // reference at the same 0.99 (VolumetricCloudRendering.cpp:1815).
        const glm::vec3 worldUp( 0.0f, 1.0f, 0.0f );
        const glm::vec3 basisUp =
             std::abs( glm::dot( lightTravel, worldUp ) ) > 0.99f ? glm::vec3( 0.0f, 0.0f, 1.0f ) : worldUp;

        const glm::vec3 lightPosition = anchor - lightTravel * lightDistanceWorld;

        const glm::mat4 lightView = glm::lookAtRH( lightPosition, anchor, basisUp );
        const glm::mat4 lightProj =
             glm::orthoRH_ZO( -extentWorld, extentWorld, -extentWorld, extentWorld, 0.0f, farPlaneWorld );

        glm::mat4 worldToMap = lightProj * lightView;

        const glm::vec4 originClip = worldToMap * glm::vec4( 0.0f, 0.0f, 0.0f, 1.0f );
        const glm::vec3 snapOffset( CloudShadowSnapToPixelGrid( originClip.x, resolution ) - originClip.x,
                                    CloudShadowSnapToPixelGrid( originClip.y, resolution ) - originClip.y, 0.0f );
        worldToMap = glm::translate( glm::mat4( 1.0f ), snapOffset ) * worldToMap;

        view.WorldToMap     = worldToMap;
        view.MapToWorld     = glm::inverse( worldToMap );
        view.LightDirection = lightTravel;
        view.FarDepthKm     = farPlaneWorld / kCloudWorldUnitsPerKm;
        view.SampleCount    = CloudShadowSampleCount( glm::dot( planetUp, toSun ), kCloudShadowBaseSamples,
                                                      kCloudShadowHorizonMultiplier );
        view.BorderFadeUv   = CloudShadowBorderFadeUv( extentKm );
        return view;
    }
} // namespace Desert::Graphic
