#pragma once

#include <Engine/Graphic/Clouds/CloudPayload.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
     * Two things break the rigidity and put the wind back into the key, i.e. back to a rebuild every frame the
     * wind moves: an authored hero cloud (placed in WORLD space, Common/CloudAuthored.glslh) and an authored
     * medium (a graph may read the world position). CloudWindIsRigid states that.
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
     * may be carried by translation at all.
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
