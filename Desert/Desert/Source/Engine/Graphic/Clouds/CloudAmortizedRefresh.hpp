#pragma once

#include <Engine/Graphic/Clouds/CloudAuthoredPayload.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Desert::Graphic
{
    /**
     * WHEN THE TWO FIELD PRODUCERS THAT DO NOT DEPEND ON THE VIEW MAY SKIP A FRAME — the cloud shadow map and
     * the sky-light occlusion volume.
     *
     * Both used to re-march the whole cloud field every frame: 512 x 512 rays of 32..64 field samples for the
     * map, 128 x 128 columns of 64 for the volume, ~55 ms and ~41 ms of a FIELD_Barn frame on MoltenVK. Neither
     * output depends on the camera's orientation, the jitter, the depth buffer or anything else that moves per
     * frame; each is a pure function of the field's inputs, the sun and (for the map) the camera's SNAPPED
     * anchor. So the work is keyed on exactly those inputs (UE does the same for its sky-light capture's
     * "real-time capture only when something moved", and Frostbite/Decima for their cloud shadow maps) and the
     * result is reused while the key holds.
     *
     * THE WIND IS THE ONE INPUT THAT MOVES EVERY FRAME, and it is not part of the key when the field moves
     * with it RIGIDLY: the procedural field and the shipped medium sample `world - windOffset` everywhere
     * (Common/CloudField.glslh, Common/CloudMediumDefault.glslh), so
     *   - the occlusion volume, which is addressed by producer and consumer alike in the wind-shifted frame
     *     (CloudSkyOcclusionVolume.shader, CloudSkyOcclusionUvw), holds the same bytes at any wind;
     *   - the shadow map, a world-space projection, holds the same bytes translated by the wind delta — which
     *     is how the consumer reads it (Graphic::CloudShadowReprojectedView) while a back copy is re-marched in
     *     bands at a frozen wind (CloudPlanShadowRefresh). The only term the translation does not carry is the
     *     planet's curvature across the delta, extentKm * |delta| / planetRadiusKm: centimetres for the metres of
     *     wind a refresh cycle spans.
     * Two things break the rigidity: an authored hero cloud (placed in WORLD space, Common/CloudAuthored.glslh)
     * and an authored medium (a graph may read the world position). CloudWindIsRigid states that.
     *
     * THE SHADOW MAP SPLITS THE FIELD ALONG THAT LINE (UE's volumetric cloud shadow: the field marched in the
     * frame that moves with the wind, the world-placed volumes a layer of their own). The two copies hold the
     * wind-rigid field ALONE — the march skips slot A (CloudShadowPush::Wind.w = 0) — so a hero cloud no longer
     * puts the wind into their key; only an authored medium does (the whole field is then non-rigid, and there
     * is no rigid part to split off). The hero clouds' part is re-marched whole on every frame it can have
     * moved (CloudShadowHeroRect): every texel whose ray can meet a hero cloud's box, into the FRONT in place,
     * through the very view the consumer reads the front with, at the current wind and with slot A. NOT a
     * second map the consumer multiplies in: a hero cloud's envelope CUTS the procedural field out
     * (Common/CloudField.glslh SampleCloudField: the max of the two producers, the procedural one scaled by
     * 1 - cutout), so its shadow is not a separable transmittance — the product of two maps would darken every
     * texel where they overlap by the deck the cutout removed. Inside the rectangle the patch IS the full
     * march; outside it no ray meets slot A, so the rigid copy IS the full march there.
     */

    /// FNV-1a over raw bytes: the key of a producer's inputs. Only trivially copyable values go in.
    class CloudRefreshKey
    {
    public:
        void Bytes( const void* data, std::size_t size )
        {
            const auto* bytes = static_cast<const unsigned char*>( data );
            for ( std::size_t i = 0; i < size; ++i )
            {
                m_Hash ^= bytes[i];
                m_Hash *= 1099511628211ull;
            }
        }

        template <class T>
        void Value( const T& value )
        {
            static_assert( std::is_trivially_copyable_v<T>, "a refresh key hashes bytes" );
            Bytes( &value, sizeof( T ) );
        }

        [[nodiscard]] uint64_t Get() const
        {
            return m_Hash;
        }

    private:
        uint64_t m_Hash = 14695981039346656037ull;
    };

    /// The field moves with the wind as one rigid body: no hero cloud and the shipped medium.
    [[nodiscard]] inline bool CloudWindIsRigid( bool hasHeroClouds, bool defaultMedium )
    {
        return !hasHeroClouds && defaultMedium;
    }

    /**
     * The field block as the two producers READ it, for the key. With the shipped medium the members only the
     * view march's lighting reads are zeroed — sun direction and colour, ambient, multiple scattering, phase,
     * fades, albedo, aerial perspective — so a sky capture that retunes the ambient does not re-march a map that
     * never read it (the shadow map's light direction reaches it through its push constant, which is keyed
     * separately). An authored medium is a graph that may read any of them, so then the block is hashed whole.
     * The accumulated wind is zeroed when the field is rigid in it (see above).
     */
    [[nodiscard]] inline CloudGpuPayload CloudFieldPayloadForKey( const CloudGpuPayload& payload,
                                                                  bool defaultMedium, bool windRigid )
    {
        CloudGpuPayload key = payload;
        if ( defaultMedium )
        {
            key.Wind.w       = 0.0f;
            key.Sun          = glm::vec4( 0.0f );
            key.SunColour    = glm::vec4( 0.0f );
            key.Ambient      = glm::vec4( 0.0f );
            key.MultiScatter = glm::vec4( 0.0f );
            key.Phase        = glm::vec4( 0.0f );
            key.Fade         = glm::vec4( 0.0f );
            key.Albedo       = glm::vec4( 0.0f );
            key.Aerial       = glm::vec3( 0.0f );
        }
        if ( windRigid )
        {
            key.Wind.x = 0.0f;
            key.Wind.y = 0.0f;
            key.Wind.z = 0.0f;
        }
        return key;
    }

    // ---- THE HERO CLOUDS' PART OF THE SHADOW MAP --------------------------------------------------------------

    /// Texels of margin the hero rectangle is grown by on every side: one for the consumer's bilinear fetch,
    /// which reads a texel's neighbour, and one for the front's wind lag (at most a texel, CloudPlanShadowRefresh),
    /// across which a texel patched on an earlier frame of this front may have left the rectangle — such a texel
    /// holds the full march of a ray that met no hero cloud then, i.e. the rigid field's own value, only while
    /// it stands at least that lag away from every box.
    inline constexpr uint32_t kCloudShadowHeroMarginTexels = 2u;

    /// A rectangle of the map's texels; empty when Width or Height is 0.
    struct CloudShadowTexelRect
    {
        uint32_t X      = 0;
        uint32_t Y      = 0;
        uint32_t Width  = 0;
        uint32_t Height = 0;

        [[nodiscard]] bool Empty() const
        {
            return Width == 0u || Height == 0u;
        }
    };

    /**
     * EVERY TEXEL WHOSE RAY CAN MEET A HERO CLOUD, as one rectangle of a @p resolution square map read through
     * @p worldToMap (orthographic down the light, so a ray is one clip-space point), grown by
     * kCloudShadowHeroMarginTexels and clipped to the map. Slot A samples only inside an instance's box
     * (CloudAuthoredInBounds, the cutout included), so outside this rectangle the field a ray marches has no
     * slot A in it at all.
     *
     * The box is in FIELD space (Common/CloudField.glslh): x and z planet-centred kilometres, y the altitude
     * above the layer's base along the radius. Its planet-centred height is bounded by the sphere — highest at
     * its top altitude over the column nearest the axis, lowest at its base altitude over the farthest corner —
     * and the 8 corners of that superset are projected. @p bottomRadiusKm is the layer's base radius
     * (CloudMakeLayer), @p planetRadiusKm the radius world y = 0 stands on.
     */
    [[nodiscard]] inline CloudShadowTexelRect CloudShadowHeroRect( const CloudAuthoredPayload& authored,
                                                                   const glm::mat4& worldToMap, float planetRadiusKm,
                                                                   float bottomRadiusKm, uint32_t resolution,
                                                                   float worldUnitsPerKm )
    {
        const int32_t count = std::clamp( authored.Count, 0, static_cast<int32_t>( kCloudAuthoredSlots ) );
        if ( count == 0 || resolution == 0u )
            return {};

        const float res = static_cast<float>( resolution );
        glm::vec2   lo( std::numeric_limits<float>::max() );
        glm::vec2   hi( std::numeric_limits<float>::lowest() );
        for ( int32_t i = 0; i < count; ++i )
        {
            const CloudAuthoredInstanceGpu& instance = authored.Instances[i];
            const glm::dvec3                boxMin( glm::vec3( instance.BoundsMin ) );
            const glm::dvec3                boxMax( glm::vec3( instance.BoundsMax ) );

            const double nearX = std::clamp( 0.0, boxMin.x, boxMax.x );
            const double nearZ = std::clamp( 0.0, boxMin.z, boxMax.z );
            const double farX  = std::max( std::abs( boxMin.x ), std::abs( boxMax.x ) );
            const double farZ  = std::max( std::abs( boxMin.z ), std::abs( boxMax.z ) );
            const double topR  = static_cast<double>( bottomRadiusKm ) + boxMax.y;
            const double baseR = std::max( static_cast<double>( bottomRadiusKm ) + boxMin.y, 0.0 );
            const double yHigh = std::sqrt( std::max( topR * topR - nearX * nearX - nearZ * nearZ, 0.0 ) );
            const double yLow  = std::sqrt( std::max( baseR * baseR - farX * farX - farZ * farZ, 0.0 ) );
            const double units = static_cast<double>( worldUnitsPerKm );
            const float  worldYLow  = static_cast<float>( ( yLow - static_cast<double>( planetRadiusKm ) ) * units );
            const float  worldYHigh = static_cast<float>( ( yHigh - static_cast<double>( planetRadiusKm ) ) * units );

            for ( uint32_t corner = 0; corner < 8u; ++corner )
            {
                const glm::vec4 world( static_cast<float>( ( ( corner & 1u ) != 0u ? boxMax.x : boxMin.x ) * units ),
                                       ( corner & 2u ) != 0u ? worldYHigh : worldYLow,
                                       static_cast<float>( ( ( corner & 4u ) != 0u ? boxMax.z : boxMin.z ) * units ),
                                       1.0f );
                const glm::vec4 clip = worldToMap * world;
                const float     w    = std::abs( clip.w ) > 1e-12f ? clip.w : 1.0f;
                // The texel whose CENTRE the point lands on, in the producer's own convention
                // (CloudShadowMap.shader main: uv = (coord + 0.5) / size).
                const glm::vec2 texel =
                     ( glm::vec2( clip.x, clip.y ) / w * 0.5f + glm::vec2( 0.5f ) ) * res - glm::vec2( 0.5f );
                lo = glm::min( lo, texel );
                hi = glm::max( hi, texel );
            }
        }

        const float margin = static_cast<float>( kCloudShadowHeroMarginTexels );
        const float x0     = std::floor( lo.x ) - margin;
        const float y0     = std::floor( lo.y ) - margin;
        const float x1     = std::ceil( hi.x ) + margin; // inclusive
        const float y1     = std::ceil( hi.y ) + margin;
        // Off the map, or a NaN corner (every compare false): no ray of the map meets slot A.
        if ( !( x1 >= 0.0f && y1 >= 0.0f && x0 <= res - 1.0f && y0 <= res - 1.0f ) )
            return {};
        CloudShadowTexelRect rect;
        rect.X      = static_cast<uint32_t>( std::max( x0, 0.0f ) );
        rect.Y      = static_cast<uint32_t>( std::max( y0, 0.0f ) );
        rect.Width  = static_cast<uint32_t>( std::min( x1, res - 1.0f ) ) - rect.X + 1u;
        rect.Height = static_cast<uint32_t>( std::min( y1, res - 1.0f ) ) - rect.Y + 1u;
        return rect;
    }

    // ---- THE SHADOW MAP'S SCHEDULE ------------------------------------------------------------------------

    /// Bands a wind-only refresh of the back copy is spread over: the steady cost is 1/this of a full march,
    /// and the front the consumer reads is at most two cycles of wind old (reprojected, so that age only
    /// matters through the curvature term above).
    inline constexpr uint32_t kCloudShadowRefreshBands = 8u;

    /// Rows one band covers, a whole number of the producer's 8-row workgroups.
    [[nodiscard]] inline uint32_t CloudShadowRefreshBandRows( uint32_t resolution, uint32_t groupRows = 8u )
    {
        const uint32_t rows = ( resolution + kCloudShadowRefreshBands - 1u ) / kCloudShadowRefreshBands;
        return std::max( groupRows, ( rows + groupRows - 1u ) / groupRows * groupRows );
    }

    /// Two copies of the map: the FRONT the lit passes read, the BACK a wind-only refresh marches into.
    struct CloudShadowRefreshState
    {
        bool      FrontBuilt = false;
        uint32_t  Front      = 0;
        uint64_t  FrontKey   = 0;
        glm::vec3 FrontWindKm{ 0.0f };

        bool      BackActive = false;
        uint64_t  BackKey    = 0;
        glm::vec3 BackWindKm{ 0.0f };
        uint32_t  BackRow = 0;
    };

    enum class CloudShadowRefreshKind : uint8_t
    {
        Reuse, // nothing marched: the front is this frame's map as it stands (translated by the wind delta)
        Full,  // the whole front re-marched at the current wind: its inputs changed
        Band,  // rows [RowBegin, RowBegin + RowCount) of the back marched at the back's frozen wind
    };

    struct CloudShadowRefreshPlan
    {
        CloudShadowRefreshKind Kind     = CloudShadowRefreshKind::Full;
        uint32_t               Target   = 0; // the copy the march writes (Full: the front, Band: the back)
        uint32_t               RowBegin = 0;
        uint32_t               RowCount = 0;
        glm::vec3              WindKm{ 0.0f }; // the wind the march is run at
    };

    /**
     * What this frame marches. @p key is every input except the wind (CloudFieldPayloadForKey, the authored
     * block, the medium, the push constant, the resolution, the sampled images); @p windRigid whether the wind
     * may be carried by translation at all (for the map: the medium is the shipped one — its copies hold the
     * field without slot A, CloudShadowHeroRect).
     *
     * @p maxLagKm bounds how far the front may be carried: one texel of the map. The translation leaves a strip
     * of that width at the upwind edge the map holds no march for (the sampler clamps it) and adds the curvature
     * term above, so a front whose wind is further behind than a texel — the map was not declared for a while
     * (clouds or casting switched off, no camera, no atmosphere) while the wind kept running, or the wind outran
     * a refresh cycle — is re-marched whole instead of being dragged across the region.
     */
    [[nodiscard]] inline CloudShadowRefreshPlan CloudPlanShadowRefresh( const CloudShadowRefreshState& state,
                                                                        uint64_t key, const glm::vec3& windKm,
                                                                        bool windRigid, uint32_t resolution,
                                                                        float maxLagKm )
    {
        CloudShadowRefreshPlan plan;
        plan.WindKm          = windKm;
        const float frontLag = glm::length( windKm - state.FrontWindKm );
        if ( !state.FrontBuilt || state.FrontKey != key || ( !windRigid && windKm != state.FrontWindKm ) ||
             frontLag > maxLagKm )
        {
            plan.Kind     = CloudShadowRefreshKind::Full;
            plan.Target   = state.Front;
            plan.RowBegin = 0;
            plan.RowCount = resolution;
            return plan;
        }
        if ( windKm == state.FrontWindKm )
        {
            plan.Kind   = CloudShadowRefreshKind::Reuse;
            plan.Target = state.Front;
            return plan;
        }
        const bool resume = state.BackActive && state.BackKey == key && state.BackRow < resolution;
        plan.Kind         = CloudShadowRefreshKind::Band;
        plan.Target       = 1u - state.Front;
        plan.RowBegin     = resume ? state.BackRow : 0u;
        plan.RowCount     = std::min( CloudShadowRefreshBandRows( resolution ), resolution - plan.RowBegin );
        plan.WindKm       = resume ? state.BackWindKm : windKm;
        return plan;
    }

    /// The plan's march was accepted by the graph: what the two copies now hold.
    inline void CloudCommitShadowRefresh( CloudShadowRefreshState& state, const CloudShadowRefreshPlan& plan,
                                          uint64_t key, uint32_t resolution )
    {
        switch ( plan.Kind )
        {
            case CloudShadowRefreshKind::Reuse:
                return;
            case CloudShadowRefreshKind::Full:
                state.FrontBuilt  = true;
                state.FrontKey    = key;
                state.FrontWindKm = plan.WindKm;
                state.BackActive  = false;
                return;
            case CloudShadowRefreshKind::Band:
                state.BackActive = true;
                state.BackKey    = key;
                state.BackWindKm = plan.WindKm;
                state.BackRow    = plan.RowBegin + plan.RowCount;
                if ( state.BackRow >= resolution )
                {
                    state.Front       = plan.Target;
                    state.FrontKey    = key;
                    state.FrontWindKm = plan.WindKm;
                    state.BackActive  = false;
                    state.BackRow     = 0;
                }
                return;
        }
    }

    /**
     * The view a consumer reads a map marched at @p builtWindKm through when the wind now stands at
     * @p windKm: the same projection carried downwind by the delta (a cloud at region point q stood at world
     * q + builtWind when the map was marched and stands at q + wind now).
     */
    template <class View>
    [[nodiscard]] View CloudShadowReprojectedView( const View& marched, const glm::vec3& builtWindKm,
                                                   const glm::vec3& windKm, float worldUnitsPerKm )
    {
        View            view  = marched;
        const glm::vec3 shift = ( windKm - builtWindKm ) * worldUnitsPerKm;
        const glm::mat4 toMap = glm::mat4( 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                           -shift.x, -shift.y, -shift.z, 1.0f );
        const glm::mat4 toWorld = glm::mat4( 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                             0.0f, shift.x, shift.y, shift.z, 1.0f );
        view.WorldToMap         = marched.WorldToMap * toMap;
        view.MapToWorld         = toWorld * marched.MapToWorld;
        return view;
    }
} // namespace Desert::Graphic
