#include <Water/Public/WaterWaveSources.hpp>

#include <Water/Public/GerstnerWaterWaves.hpp>
#include <Water/Public/WaterBodyComponent.hpp>
#include <Water/Public/WaterBodyGather.hpp>
#include <Water/Public/WaterWavesService.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/System/WindField.hpp>
#include <Engine/ECS/TagComponent.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>

#include <glm/vec3.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace Desert::ECS
{
    namespace
    {
        std::string NameOf( const entt::registry& registry, const entt::entity entity )
        {
            const auto* const tag = registry.try_get<TagComponent>( entity );
            return tag != nullptr ? tag->Tag : std::to_string( static_cast<uint32_t>( entity ) );
        }

        // splitmix64's finaliser: two refused sets that differ by one body differ in every bit of the key.
        uint64_t Mix( uint64_t value )
        {
            value = ( value ^ ( value >> 30u ) ) * 0xbf58476d1ce4e5b9ull;
            value = ( value ^ ( value >> 27u ) ) * 0x94d049bb133111ebull;
            return value ^ ( value >> 31u );
        }
    } // namespace

    WaterWaveSources::WaterWaveSources()
    {
        Reset();
    }

    void WaterWaveSources::Reset()
    {
        m_Rows.clear();
        m_Answers.clear();
        m_Lanes.clear();
        m_Revision    = 0;
        m_ShapeSerial = 0;
        m_RefusedKey  = 0;
        m_Said.clear();
        m_SaidOwner   = entt::null;
        m_SaidTooMany = false;
        m_Surface     = std::make_shared<Water::WaterFFTSurface>();
        m_Table       = std::make_shared<const Water::WaterWaveTable>(
             Water::WaterWaveTable{ .Lanes = {}, .Surface = m_Surface } );
    }

    void WaterWaveSources::SceneLoaded( const entt::registry& registry )
    {
        m_Said.clear();
        m_SaidOwner         = entt::null;
        m_SaidTooMany       = false;
        m_Revision          = 0; // decided again, and said again, by the next Refresh
        auto* const service = Runtime::GetWaterWavesService();
        for ( const entt::entity entity : registry.view<const WaterBodyComponent>() )
            if ( const Assets::AssetHandle waves = registry.get<WaterBodyComponent>( entity ).Data.WaterWaves;
                 static_cast<uint64_t>( waves ) != 0 )
                (void)service->KindOf( waves );
    }

    void WaterWaveSources::Refresh( entt::registry& registry )
    {
        StateWinds( registry );

        m_Seen.clear();
        const auto view = registry.view<WaterBodyComponent, TransformComponent>();
        for ( const entt::entity entity : view )
        {
            const auto* const id = registry.try_get<UUIDComponent>( entity );
            m_Seen.push_back(
                 Row{ .Entity = entity,
                      .Waves  = static_cast<uint64_t>( view.get<WaterBodyComponent>( entity ).Data.WaterWaves ),
                      .UUID   = id != nullptr ? static_cast<uint64_t>( id->UUID )
                                              : std::numeric_limits<uint64_t>::max() } );
        }
        std::sort( m_Seen.begin(), m_Seen.end(),
                   []( const Row& a, const Row& b ) { return a.Entity < b.Entity; } );

        // The membership, each body's set (edited in place, without a signal) and what the sets are.
        const uint64_t revision = WaterBodyRevisionOf( registry );
        const uint64_t serial   = Runtime::GetWaterWavesService()->ShapeSerial();
        if ( revision != m_Revision || serial != m_ShapeSerial || m_Seen != m_Rows )
        {
            m_Rows        = m_Seen;
            m_Revision    = revision;
            m_ShapeSerial = serial;
            Decide( registry );
        }
        BuildLanes( registry );
    }

    // THE OCEAN'S WIND (WATER-UNDERWATER): one truth, the scene's ECS::WindAt - the query foliage, clouds, cloth
    // and groom ask - at the first visible body naming each wave set (its wave origin, at its still level). A wave
    // set holds no wind of its own; WaterWavesService rebuilds the set's waves when this changes, so a WindSource
    // edit moves the sea without a reload. Every frame, whether or not a zone draws the body: what floats floats
    // on it.
    void WaterWaveSources::StateWinds( entt::registry& registry )
    {
        auto* const service = Runtime::GetWaterWavesService();
        m_WindStated.clear();
        const auto view = registry.view<WaterBodyComponent, TransformComponent>();
        for ( const entt::entity entity : view )
        {
            const Assets::AssetHandle waves = view.get<WaterBodyComponent>( entity ).Data.WaterWaves;
            if ( static_cast<uint64_t>( waves ) == 0 || IsHidden( registry, entity ) ||
                 std::find( m_WindStated.begin(), m_WindStated.end(), waves ) != m_WindStated.end() )
                continue;
            m_WindStated.push_back( waves );
            const glm::vec3 wind =
                 WindAt( registry, view.get<TransformComponent>( entity ).GetTranslation() ).Velocity;
            service->SetWind( waves, glm::vec2( wind.x, wind.z ) );
        }
    }

    void WaterWaveSources::Decide( const entt::registry& registry )
    {
        auto* const service = Runtime::GetWaterWavesService();
        m_Answers.clear();
        const Row*           owner     = nullptr;
        Runtime::WaveSetKind ownerKind = Runtime::WaveSetKind::Pending;
        for ( const Row& row : m_Rows )
        {
            Answer answer{ .Entity = row.Entity, .Is = Choice::Flat, .Lane = -1 };
            if ( row.Waves != 0 )
            {
                const Runtime::WaveSetKind kind = service->KindOf( Assets::AssetHandle( row.Waves ) );
                if ( kind == Runtime::WaveSetKind::Pending )
                    answer.Is = Choice::Wait;
                else if ( kind == Runtime::WaveSetKind::Gerstner )
                    answer.Is = Choice::Gerstner;
                else if ( kind == Runtime::WaveSetKind::FFT )
                    answer.Is = Choice::Ocean;
                if ( ( kind == Runtime::WaveSetKind::Pending || kind == Runtime::WaveSetKind::FFT ) &&
                     ( owner == nullptr || row.UUID < owner->UUID ) )
                {
                    owner     = &row;
                    ownerKind = kind;
                }
            }
            m_Answers.push_back( answer );
        }

        std::vector<entt::entity> refused;
        for ( size_t i = 0; owner != nullptr && i < m_Rows.size(); ++i )
        {
            if ( m_Answers[i].Is != Choice::Ocean || m_Rows[i].Waves == owner->Waves )
                continue;
            if ( ownerKind == Runtime::WaveSetKind::Pending )
            {
                m_Answers[i].Is = Choice::Wait;
                continue;
            }
            m_Answers[i].Is = Choice::Refused;
            refused.push_back( m_Rows[i].Entity );
        }

        m_RefusedKey = 0;
        for ( const entt::entity entity : refused )
            m_RefusedKey = Mix( m_RefusedKey + 0x9e3779b97f4a7c15ull + static_cast<uint32_t>( entity ) );

        entt::entity said = entt::null;
        if ( !refused.empty() )
            said = owner->Entity;
        if ( refused == m_Said && said == m_SaidOwner )
            return;
        for ( const entt::entity entity : refused )
            LOG_ERROR(
                 "[Water] '{}' is no water body: it names another FFT wave set than '{}'{}, and a world has one "
                 "ocean - give both the same WaterWaves, or this one a Gerstner set",
                 NameOf( registry, entity ), NameOf( registry, owner->Entity ),
                 IsHidden( registry, owner->Entity ) ? " (hidden)" : "" );
        m_Said      = std::move( refused );
        m_SaidOwner = said;
    }

    void WaterWaveSources::BuildLanes( const entt::registry& registry )
    {
        auto* const service = Runtime::GetWaterWavesService();
        const bool  fftTier =
             Common::Scalability::QualityState::Resolved().As<Common::Scalability::WaterWaveMethod>(
                  Common::Scalability::Parameter::EffectsWaterWaveMethod ) ==
             Common::Scalability::WaterWaveMethod::FFT;
        const bool drawable =
             m_Surface->Verdict.load( std::memory_order_acquire ) == Water::WaterFFTVerdict::Drawable;

        m_Lanes.clear();
        bool tooMany = false;
        // The visible bodies first: a hidden one never takes a drawn one's lane.
        for ( const bool hidden : { false, true } )
            for ( Answer& answer : m_Answers )
            {
                if ( IsHidden( registry, answer.Entity ) != hidden )
                    continue;
                answer.Lane = -1;
                if ( answer.Is == Choice::Flat || answer.Is == Choice::Refused )
                    continue;
                const Assets::AssetHandle waves =
                     registry.get<WaterBodyComponent>( answer.Entity ).Data.WaterWaves;
                const glm::vec3 location = registry.get<TransformComponent>( answer.Entity ).GetTranslation();
                const glm::vec2 origin( location.x, location.z );
                const auto      found =
                     std::find_if( m_Lanes.begin(), m_Lanes.end(), [&]( const Water::WaterWaveLane& lane )
                                   { return lane.Asset == waves && lane.Origin == origin; } );
                if ( found != m_Lanes.end() )
                {
                    answer.Lane = static_cast<int32_t>( found - m_Lanes.begin() );
                    continue;
                }
                if ( m_Lanes.size() >= Water::kMaxWaveLanes )
                {
                    tooMany = true;
                    if ( !m_SaidTooMany )
                        LOG_ERROR(
                             "[Water] '{}' has flat water: a world draws {} wave lanes (a wave set at a wave "
                             "origin), and its own is past them",
                             NameOf( registry, answer.Entity ), Water::kMaxWaveLanes );
                    continue;
                }
                Water::WaterWaveLane lane;
                lane.Asset  = waves;
                lane.Origin = origin;
                if ( answer.Is != Choice::Wait )
                {
                    if ( const auto built = service->Get( waves ) )
                        lane.Waves = built.GetValue();
                    if ( answer.Is == Choice::Ocean && fftTier )
                        if ( const auto spectrum = service->GetSpectrum( waves ) )
                            lane.Spectrum = spectrum.GetValue();
                    if ( lane.Spectrum != nullptr && drawable )
                        lane.State = Water::WaterWaveState::FFT;
                    else if ( lane.Waves != nullptr )
                        lane.State = Water::WaterWaveState::Gerstner;
                }
                answer.Lane = static_cast<int32_t>( m_Lanes.size() );
                m_Lanes.push_back( std::move( lane ) );
            }
        m_SaidTooMany = tooMany;
        // A steady frame keeps the table it has.
        if ( m_Lanes != m_Table->Lanes )
            m_Table = std::make_shared<const Water::WaterWaveTable>(
                 Water::WaterWaveTable{ .Lanes = m_Lanes, .Surface = m_Surface } );
    }

    const WaterWaveSources::Answer* WaterWaveSources::Find( const entt::entity entity ) const noexcept
    {
        const auto found =
             std::lower_bound( m_Answers.begin(), m_Answers.end(), entity,
                               []( const Answer& a, const entt::entity e ) { return a.Entity < e; } );
        return found != m_Answers.end() && found->Entity == entity ? &*found : nullptr;
    }

    int32_t WaterWaveSources::LaneOf( const entt::entity entity ) const noexcept
    {
        const Answer* const answer = Find( entity );
        return answer != nullptr ? answer->Lane : -1;
    }

    bool WaterWaveSources::IsRefused( const entt::entity entity ) const noexcept
    {
        const Answer* const answer = Find( entity );
        return answer != nullptr && answer->Is == Choice::Refused;
    }
} // namespace Desert::ECS
