#include <Water/Public/WaterShoreFieldSource.hpp>

#include <Water/Public/WaterBodyComponent.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Landscape/Public/LandscapeMaterialComponent.hpp>
#include <Landscape/Public/LandscapeRootOf.hpp>
#include <LandscapeCore/Public/LandscapeData.hpp>

#include <glm/common.hpp>

#include <algorithm>
#include <bit>
#include <chrono>

namespace Desert::ECS
{
    namespace
    {
        class Key
        {
        public:
            void Mix( const uint64_t v )
            {
                m_Value = ( m_Value ^ v ) * 1099511628211ull;
            }
            void MixFloat( const float v )
            {
                Mix( static_cast<uint64_t>( std::bit_cast<uint32_t>( v ) ) );
            }
            [[nodiscard]] uint64_t Value() const
            {
                return m_Value;
            }

        private:
            uint64_t m_Value = 1469598103934665603ull;
        };

        // Every visible body's footprint. A river's is its ribbon's box; it has a floor but no still shore.
        void GatherLevels( entt::registry& registry, std::vector<Water::WaterBodyLevel>& out )
        {
            out.clear();
            for ( const entt::entity entity : registry.view<WaterBodyComponent, TransformComponent>() )
            {
                if ( IsHidden( registry, entity ) )
                    continue;
                const WaterBodyData& data   = registry.get<WaterBodyComponent>( entity ).Data;
                const glm::vec3      at     = registry.get<TransformComponent>( entity ).GetTranslation();
                const glm::vec2      centre = glm::vec2( at.x, at.z );
                glm::vec2            lo     = centre - 0.5f * data.OceanExtents;
                glm::vec2            hi     = centre + 0.5f * data.OceanExtents;
                float                level  = at.y;
                const bool           river  = data.Type == Water::WaterBodyType::River;
                if ( river )
                {
                    const auto points = RiverPointsOf( data );
                    if ( points.empty() )
                        continue;
                    lo = hi = centre + glm::vec2( points[0].x, points[0].z );
                    for ( const glm::vec3& p : points )
                    {
                        lo    = glm::min( lo, centre + glm::vec2( p.x, p.z ) );
                        hi    = glm::max( hi, centre + glm::vec2( p.x, p.z ) );
                        level = std::min( level, at.y + p.y );
                    }
                    // The banks, and the little a bend swings out of its points' box.
                    lo -= 2.0f * data.RiverWidth;
                    hi += 2.0f * data.RiverWidth;
                }
                out.push_back( { glm::vec4( lo, hi ), level, !river } );
            }
        }
    } // namespace

    Common::ResultStr<WaterShoreFieldSource::Field> WaterShoreFieldSource::Bake( const Input& input )
    {
        std::vector<World::Landscape::LandscapeTileData> heights;
        std::vector<World::Landscape::LandscapeRayTile>  tiles;
        heights.reserve( input.Tiles.size() ); // the ray tiles point into it
        for ( const Tile& tile : input.Tiles )
        {
            auto data =
                 World::Landscape::LandscapeTileData::FromSamples( tile.SamplesX, tile.SamplesZ, tile.Samples );
            if ( !data )
                return Common::MakeError<Field>( "water shore field: " + data.GetError() );
            heights.push_back( data.ExtractValue() );
            tiles.push_back( { &heights.back(), tile.Frame } );
        }
        return Water::BakeWaterShoreField( tiles, input.Bodies, input.SpacingCm, input.Base.get() );
    }

    void WaterShoreFieldSource::BakeJob::Run() noexcept
    {
        Promise.set_value( Bake( World ) );
    }

    void WaterShoreFieldSource::AdoptDone()
    {
        Common::ResultStr<Field> baked = m_Job->Done.get();
        m_Job                          = nullptr;
        if ( baked )
            m_Field = baked.ExtractValue();
        else
            LOG_ERROR( "[Water] {}", baked.GetError() );
    }

    float WaterShoreFieldSource::RootOffsetOf( entt::registry& registry, const Common::UUID& root )
    {
        const entt::entity entity  = FindLandscapeRootEntity( registry, root );
        auto* const        service = Runtime::ResourceRegistry::GetMaterialService();
        const auto* const  material =
             entity != entt::null ? registry.try_get<LandscapeMaterialComponent>( entity ) : nullptr;
        if ( service == nullptr || material == nullptr )
            return 0.0f; // the program's own default: no displacement is drawn
        const RootOffset now{ .Material     = static_cast<uint64_t>( material->Data.Material ),
                              .Invalidation = service->GetInvalidationVersion(),
                              .Values       = service->GetValuesEditSerial(),
                              .Content      = Assets::ContentRegistry::WriteSerial() };
        RootOffset&      cached = m_Roots[static_cast<uint64_t>( root )];
        if ( cached.Resolved && cached.Material == now.Material && cached.Invalidation == now.Invalidation &&
             cached.Values == now.Values && cached.Content == now.Content )
            return cached.OffsetCm;
        Graphic::MaterialOverrides overrides;
        if ( now.Material == 0u || !service->ResolveOverrides( material->Data.Material, overrides ) )
            return 0.0f; // LandscapeECSSystem names it; asked again at the next read
        cached          = now;
        cached.Resolved = true;
        // Terrain.shader's own defaults; the mean of a displacement map is the middle of its range.
        float magnitude = 0.0f;
        float centre    = 0.5f;
        for ( const auto& [name, value] : overrides.Params )
        {
            if ( name == "DisplacementMagnitude" )
                magnitude = value.x;
            else if ( name == "DisplacementCenter" )
                centre = value.x;
        }
        cached.OffsetCm = magnitude * ( 0.5f - centre );
        return cached.OffsetCm;
    }

    void WaterShoreFieldSource::ResolveRootOffsets( entt::registry& registry )
    {
        m_Offsets.clear();
        for ( const LandscapeTileRef& tile : DrawableLandscapeTiles( registry ).Tiles )
        {
            const Common::UUID root( tile.Component->Landscape );
            if ( !m_Offsets.contains( static_cast<uint64_t>( root ) ) )
                m_Offsets.emplace( static_cast<uint64_t>( root ), RootOffsetOf( registry, root ) );
        }
    }

    bool WaterShoreFieldSource::ReadWorld( entt::registry& registry )
    {
        GatherLevels( registry, m_Bodies );
        if ( m_Bodies.empty() )
        {
            m_Field = nullptr;
            m_Job   = nullptr; // its worker finishes into a promise nobody reads
            m_Key   = 0;
            return false;
        }
        Key key;
        for ( const Water::WaterBodyLevel& body : m_Bodies )
        {
            for ( int i = 0; i < 4; ++i )
                key.MixFloat( body.FootprintMinMax[i] );
            key.MixFloat( body.StillLevelCm );
            key.Mix( body.HasShore ? 1u : 0u );
        }
        ResolveRootOffsets( registry );
        for ( const LandscapeTileRef& tile : DrawableLandscapeTiles( registry ).Tiles )
        {
            key.Mix( tile.Component->Heights->HeightStamp() );
            key.MixFloat( tile.Frame.OriginX );
            key.MixFloat( tile.Frame.OriginZ );
            key.MixFloat( tile.Frame.BaseY );
            key.MixFloat( tile.Frame.SpacingCm );
            key.MixFloat( tile.Frame.ZScale );
            key.MixFloat( m_Offsets.at( static_cast<uint64_t>( Common::UUID( tile.Component->Landscape ) ) ) );
        }
        m_WorldKey = key.Value();
        return true;
    }

    WaterShoreFieldSource::Input WaterShoreFieldSource::CopyInput( entt::registry& registry )
    {
        Input input;
        input.Bodies = m_Bodies;
        input.Base   = m_Field;
        for ( const LandscapeTileRef& tile : DrawableLandscapeTiles( registry ).Tiles )
        {
            const World::Landscape::LandscapeTileData& heights = *tile.Component->Heights;
            Tile copy{ heights.Samples(), heights.SamplesX(), heights.SamplesZ(), tile.Frame };
            copy.Frame.BaseY += m_Offsets.at( static_cast<uint64_t>( Common::UUID( tile.Component->Landscape ) ) );
            input.Tiles.push_back( std::move( copy ) );
            input.SpacingCm = input.SpacingCm == 0.0f ? tile.Frame.SpacingCm
                                                      : std::min( input.SpacingCm, tile.Frame.SpacingCm );
        }
        return input;
    }

    bool WaterShoreFieldSource::Refresh( entt::registry& registry )
    {
        if ( m_Job != nullptr && m_Job->Done.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
            AdoptDone();
        if ( !ReadWorld( registry ) )
            return false;
        // A refused world keeps its key: it is baked again when it changes, not every step.
        if ( m_WorldKey == m_Key || m_Job != nullptr )
            return true;
        m_Key        = m_WorldKey;
        m_Job        = std::make_shared<BakeJob>();
        m_Job->World = CopyInput( registry );
        // Refused while the job system is going down: the bake stays unclaimed, and UpdateNow may still take it.
        (void)Common::JobSystem::Get().Submit(
             [job = m_Job]() noexcept
             {
                 if ( !job->Claimed.test_and_set() )
                     job->Run();
             } );
        return true;
    }

    WaterShoreFieldSource::Field WaterShoreFieldSource::Update( entt::registry& registry )
    {
        const std::scoped_lock lock( m_Mutex );
        return Refresh( registry ) ? m_Field : nullptr;
    }

    WaterShoreFieldSource::Field WaterShoreFieldSource::UpdateNow( entt::registry& registry )
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !Refresh( registry ) )
            return nullptr;
        if ( m_Field == nullptr && m_Job != nullptr )
        {
            if ( !m_Job->Claimed.test_and_set() )
                m_Job->Run();
            AdoptDone(); // else a worker is inside it; the bake waits for nothing
        }
        return m_Field;
    }
} // namespace Desert::ECS
