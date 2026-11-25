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
        return Water::BakeWaterShoreField( tiles, input.Bodies, input.SpacingCm );
    }

    void WaterShoreFieldSource::Adopt( Common::ResultStr<Field> baked )
    {
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
        if ( cached.Material == now.Material && cached.Invalidation == now.Invalidation &&
             cached.Values == now.Values && cached.Content == now.Content )
            return cached.OffsetCm;
        cached = now;
        Graphic::MaterialOverrides overrides;
        if ( now.Material == 0u || !service->ResolveOverrides( material->Data.Material, overrides ) )
            return 0.0f; // LandscapeECSSystem names the material it could not resolve
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

    bool WaterShoreFieldSource::ReadWorld( entt::registry& registry )
    {
        GatherLevels( registry, m_Bodies );
        if ( m_Bodies.empty() )
        {
            m_Field = nullptr;
            m_Job   = {};
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
        for ( const LandscapeTileRef& tile : DrawableLandscapeTiles( registry ).Tiles )
        {
            World::Landscape::LandscapeTileData& heights = *tile.Component->Heights;
            if ( !m_Job.valid() && !heights.DirtyRects( World::Landscape::LandscapeDirtyConsumer::Water ).empty() )
            {
                (void)heights.TakeDirtyRects( World::Landscape::LandscapeDirtyConsumer::Water );
                ++m_Edits;
            }
            key.Mix( static_cast<uint64_t>( reinterpret_cast<uintptr_t>( &heights ) ) );
            key.MixFloat( tile.Frame.OriginX );
            key.MixFloat( tile.Frame.OriginZ );
            key.MixFloat( tile.Frame.BaseY );
            key.MixFloat( tile.Frame.SpacingCm );
            key.MixFloat( tile.Frame.ZScale );
            key.MixFloat( RootOffsetOf( registry, Common::UUID( tile.Component->Landscape ) ) );
        }
        key.Mix( m_Edits );
        m_WorldKey = key.Value();
        return true;
    }

    WaterShoreFieldSource::Input WaterShoreFieldSource::CopyInput( entt::registry& registry )
    {
        Input input;
        input.Bodies = m_Bodies;
        for ( const LandscapeTileRef& tile : DrawableLandscapeTiles( registry ).Tiles )
        {
            const World::Landscape::LandscapeTileData& heights = *tile.Component->Heights;
            Tile copy{ heights.Samples(), heights.SamplesX(), heights.SamplesZ(), tile.Frame };
            copy.Frame.BaseY += RootOffsetOf( registry, Common::UUID( tile.Component->Landscape ) );
            input.Tiles.push_back( std::move( copy ) );
            input.SpacingCm = input.SpacingCm == 0.0f ? tile.Frame.SpacingCm
                                                      : std::min( input.SpacingCm, tile.Frame.SpacingCm );
        }
        return input;
    }

    WaterShoreFieldSource::Field WaterShoreFieldSource::Update( entt::registry& registry )
    {
        const std::scoped_lock lock( m_Mutex );
        if ( m_Job.valid() && m_Job.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
            Adopt( m_Job.get() );
        if ( !ReadWorld( registry ) )
            return nullptr;
        if ( m_WorldKey != m_Key && !m_Job.valid() )
        {
            m_Key = m_WorldKey;
            m_Job = Common::JobSystem::Get().Async( [input = CopyInput( registry )]() noexcept
                                                    { return Bake( input ); } );
        }
        return m_Field;
    }

    WaterShoreFieldSource::Field WaterShoreFieldSource::UpdateNow( entt::registry& registry )
    {
        if ( Field field = Update( registry ) )
            return field;
        const std::scoped_lock lock( m_Mutex );
        if ( m_Field == nullptr && !m_Bodies.empty() )
            Adopt( Bake( CopyInput( registry ) ) );
        return m_Field;
    }
} // namespace Desert::ECS
