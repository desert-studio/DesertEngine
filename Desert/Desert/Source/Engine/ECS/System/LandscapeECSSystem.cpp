#include "LandscapeECSSystem.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/Render/Commands/DrawLandscapeTileCommand.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Common/Core/Logger.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace Desert::ECS
{
    namespace
    {
        namespace Landscape = World::Landscape;

        // The root a tile names, in the frame LandscapeLayout computes with — the same construction the
        // scene loader validates tiles against (SceneSerializer.cpp, FindLandscapeRoot).
        std::optional<Landscape::LandscapeRoot> FindRoot( entt::registry& registry, const Common::UUID& id )
        {
            if ( id == Common::UUID::Null() )
                return std::nullopt;
            for ( const auto entity : registry.view<LandscapeComponent, UUIDComponent>() )
            {
                if ( registry.get<UUIDComponent>( entity ).UUID != id )
                    continue;
                const auto&              component = registry.get<LandscapeComponent>( entity );
                Landscape::LandscapeRoot root;
                root.Origin       = glm::vec3( Entity( entity, registry ).GetWorldTransform()[3] );
                root.QuadsPerTile = component.QuadsPerTile;
                root.SpacingCm    = component.SpacingCm;
                root.ZScale       = component.ZScale;
                return root;
            }
            return std::nullopt;
        }

        std::shared_ptr<Graphic::Image2D> UploadHeightmap( const Landscape::LandscapeTileData& tile, int32_t tileX,
                                                           int32_t tileZ )
        {
            // The samples as the CPU holds them, row-major z * SamplesX + x — which is the image's own
            // row order, so the byte copy IS the upload layout (R16 texel = one uint16, host byte order,
            // which is the device's on every platform this builds for).
            const std::vector<uint16_t>& samples = tile.Samples();
            std::vector<unsigned char>   bytes( samples.size() * sizeof( uint16_t ) );
            std::memcpy( bytes.data(), samples.data(), bytes.size() );

            Core::Formats::Image2DSpecification spec = {
                 .Tag        = "LandscapeTile(" + std::to_string( tileX ) + "," + std::to_string( tileZ ) + ")",
                 .Width      = tile.SamplesX(),
                 .Height     = tile.SamplesZ(),
                 .Format     = Core::Formats::ImageFormat::R16_UNORM,
                 .Mips       = 1,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::ImageProperties::Sample,
            };
            spec.Data = std::move( bytes );

            // A derived copy: the recipe is the component's samples, which the scene owns, so the owner is
            // the renderer side that re-derives it — releasing it loses nothing.
            const Graphic::ResourceAttributionScope owned( Graphic::ResourceOwner::SceneRenderer );
            return Graphic::Image2D::Create( spec );
        }
    } // namespace

    LandscapeECSSystem::~LandscapeECSSystem() = default;

    void LandscapeECSSystem::Update( entt::registry&                       registry,
                                     Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                                     const Common::Timestep& /*ts*/ )
    {
        auto view = registry.view<LandscapeTileComponent>();
        for ( const auto entity : view )
        {
            auto& tileComp = view.get<LandscapeTileComponent>( entity );
            if ( !tileComp.Heights.has_value() )
                continue; // not loaded, or refused at load (the loader said why); the sweep below frees it
            Landscape::LandscapeTileData& heights = *tileComp.Heights;

            const auto root = FindRoot( registry, tileComp.Landscape );
            const auto fits = root ? Landscape::CheckTileMatchesRoot( heights, *root )
                                   : Common::MakeError<bool>( "its root is not loaded or is not a landscape" );
            if ( !fits.IsSuccess() )
            {
                if ( m_Warned.insert( entity ).second )
                    LOG_WARN( "[Landscape] tile ({}, {}) is not drawn: {}", tileComp.TileX, tileComp.TileZ,
                              fits.GetError() );
                continue;
            }
            m_Warned.erase( entity );

            // ALWAYS taken, drawn or hidden: the list is "what the GPU copy does not have yet", and a hidden
            // tile's copy is refreshed like any other so showing it again shows the current heights.
            const bool dirty = !heights.TakeDirtyRects().empty();
            TileGpu&   gpu   = m_Tiles[entity];
            if ( dirty || !gpu.Heightmap || gpu.SamplesX != heights.SamplesX() ||
                 gpu.SamplesZ != heights.SamplesZ() )
            {
                gpu.Heightmap = UploadHeightmap( heights, tileComp.TileX, tileComp.TileZ );
                gpu.SamplesX  = heights.SamplesX();
                gpu.SamplesZ  = heights.SamplesZ();
                if ( !gpu.Heightmap )
                {
                    LOG_ERROR( "[Landscape] tile ({}, {}): the {} x {} R16 heightmap could not be created; the "
                               "tile is not drawn",
                               tileComp.TileX, tileComp.TileZ, gpu.SamplesX, gpu.SamplesZ );
                    m_Tiles.erase( entity );
                    continue;
                }
            }

            if ( IsHidden( registry, entity ) )
                continue;

            Graphic::System::LandscapeTileDraw draw;
            draw.OriginX      = root->Origin.x;
            draw.OriginZ      = root->Origin.z;
            draw.BaseY        = root->Origin.y;
            draw.SpacingCm    = root->SpacingCm;
            draw.ZScale       = root->ZScale;
            draw.FirstSampleX = tileComp.TileX * static_cast<int32_t>( root->QuadsPerTile );
            draw.FirstSampleZ = tileComp.TileZ * static_cast<int32_t>( root->QuadsPerTile );
            draw.QuadsPerTile = root->QuadsPerTile;
            renderCommandBuffer.Emplace<Graphic::Render::DrawLandscapeTileCommand>( gpu.Heightmap.get(), draw );
        }

        // Release: the entity is gone, lost its tile component, or its heights were unloaded.
        for ( auto it = m_Tiles.begin(); it != m_Tiles.end(); )
        {
            const bool alive = registry.valid( it->first ) && registry.has<LandscapeTileComponent>( it->first ) &&
                               registry.get<LandscapeTileComponent>( it->first ).Heights.has_value();
            it = alive ? std::next( it ) : m_Tiles.erase( it );
        }
        for ( auto it = m_Warned.begin(); it != m_Warned.end(); )
            it = registry.valid( *it ) ? std::next( it ) : m_Warned.erase( it );
    }
} // namespace Desert::ECS
