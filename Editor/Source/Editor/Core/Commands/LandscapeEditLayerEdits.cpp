#include "LandscapeEditLayerEdits.hpp"

#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Common/Core/Core.hpp>

#include <algorithm>
#include <format>

namespace Desert::Editor::Commands
{
    namespace WL = World::Landscape;

    Common::ResultStr<std::vector<entt::entity>>
    LandscapeLoadedTiles( entt::registry& registry, const Common::UUID& landscape, bool refuseUnloaded )
    {
        std::vector<entt::entity> tiles;
        for ( const auto entity : registry.view<ECS::LandscapeTileComponent>() )
        {
            const auto& tile = registry.get<ECS::LandscapeTileComponent>( entity );
            if ( tile.Landscape != landscape )
                continue;
            if ( tile.Heights.has_value() )
                tiles.push_back( entity );
            else if ( refuseUnloaded )
                return Common::MakeFormattedError<std::vector<entt::entity>>(
                     "landscape edit layers: tile {}_{} is not loaded — its stored samples would keep the old "
                     "merge",
                     tile.TileX, tile.TileZ );
        }
        return Common::MakeSuccess( std::move( tiles ) );
    }

    Common::ResultStr<size_t> LandscapeEditLayerIndex( const WL::LandscapeEditLayerStack& stack,
                                                       const Common::UUID&                layer )
    {
        for ( size_t i = 0; i < stack.Layers.size(); ++i )
            if ( static_cast<uint64_t>( stack.Layers[i].Guid ) == static_cast<uint64_t>( layer ) )
                return Common::MakeSuccess( i );
        return Common::MakeFormattedError<size_t>( "landscape edit layers: the landscape has no layer {}",
                                                   static_cast<uint64_t>( layer ) );
    }

    WL::LandscapeEditLayerStack LandscapeStackWithLayerAdded( const WL::LandscapeEditLayerStack& stack,
                                                              const Common::UUID&                editing,
                                                              const Common::UUID&                guid )
    {
        WL::LandscapeEditLayer layer;
        layer.Guid = guid;
        for ( size_t n = stack.Layers.size() + 1;; ++n )
        {
            layer.Name = std::format( "Layer {}", n );
            if ( std::ranges::none_of( stack.Layers,
                                       [&]( const WL::LandscapeEditLayer& l ) { return l.Name == layer.Name; } ) )
                break;
        }
        const auto below = LandscapeEditLayerIndex( stack, editing );
        const auto at    = stack.Layers.empty() ? 0u : ( below ? below.GetValue() : 0u ) + 1u;
        auto       after = stack;
        after.Layers.insert( after.Layers.begin() + static_cast<std::ptrdiff_t>( at ), std::move( layer ) );
        return after;
    }

    Common::ResultStr<WL::LandscapeEditLayerStack>
    LandscapeStackWithLayerRemoved( const WL::LandscapeEditLayerStack& stack, const Common::UUID& layer )
    {
        const auto index = LandscapeEditLayerIndex( stack, layer );
        if ( !index )
            return Common::MakeError<WL::LandscapeEditLayerStack>( index.GetError() );
        if ( stack.Layers.size() == 1u )
            return Common::MakeFormattedError<WL::LandscapeEditLayerStack>(
                 "landscape edit layers: '{}' is the landscape's last layer — a landscape has at least one",
                 stack.Layers.front().Name );
        auto after = stack;
        after.Layers.erase( after.Layers.begin() + static_cast<std::ptrdiff_t>( index.GetValue() ) );
        return Common::MakeSuccess( std::move( after ) );
    }

    Common::ResultStr<std::vector<LandscapeRemovedTileLayer>>
    LandscapeEditLayerTileDataOf( entt::registry& registry, const Common::UUID& landscape,
                                  const Common::UUID& layer )
    {
        using Result = std::vector<LandscapeRemovedTileLayer>;
        auto tiles   = LandscapeLoadedTiles( registry, landscape, true );
        if ( !tiles )
            return Common::MakeError<Result>( tiles.GetError() );
        Result data;
        for ( const auto entity : tiles.GetValue() )
        {
            const auto& tile = registry.get<ECS::LandscapeTileComponent>( entity );
            if ( !tile.Heights )
                return Common::MakeFormattedError<Result>( "landscape edit layers: tile {}_{} lost its samples",
                                                           tile.TileX, tile.TileZ );
            if ( const auto* held = tile.Heights->FindEditLayer( layer ) )
                data.push_back( { tile.TileX, tile.TileZ, *held } );
        }
        return Common::MakeSuccess( std::move( data ) );
    }

    Common::BoolResultStr ApplyLandscapeEditLayerStack( entt::registry& registry, const Common::UUID& landscape,
                                                        const WL::LandscapeEditLayerStack&         stack,
                                                        std::span<const WL::LandscapeLayerRule>    rules,
                                                        std::span<const LandscapeRemovedTileLayer> restore,
                                                        bool                                       merge )
    {
        if ( auto ok = WL::ValidateLandscapeEditLayerStack( stack ); !ok )
            return Common::MakeFormattedError<bool>( "landscape edit layers: {}", ok.GetError() );
        const auto root = ECS::FindLandscapeRootEntity( registry, landscape );
        if ( root == entt::null )
            return Common::MakeError<bool>( "landscape edit layers: the landscape root is not loaded" );
        auto tiles = LandscapeLoadedTiles( registry, landscape, merge );
        if ( !tiles )
            return Common::MakeError<bool>( tiles.GetError() );
        registry.get<ECS::LandscapeComponent>( root ).EditLayers = stack;
        for ( const auto entity : tiles.GetValue() )
        {
            auto& tile = registry.get<ECS::LandscapeTileComponent>( entity );
            if ( !tile.Heights )
                return Common::MakeFormattedError<bool>( "landscape edit layers: tile {}_{} lost its samples",
                                                         tile.TileX, tile.TileZ );
            auto&                     data = *tile.Heights;
            std::vector<Common::UUID> stale;
            for ( const auto& held : data.EditLayers() )
                if ( stack.Find( held.Layer ) == nullptr )
                    stale.push_back( held.Layer );
            for ( const auto& guid : stale )
                data.RemoveEditLayer( guid );
            for ( const auto& removed : restore )
                if ( removed.TileX == tile.TileX && removed.TileZ == tile.TileZ )
                    if ( auto ok = data.SetEditLayer( removed.Data ); !ok )
                        return Common::MakeFormattedError<bool>( "landscape edit layers: tile {}_{}: {}",
                                                                 tile.TileX, tile.TileZ, ok.GetError() );
            if ( !merge )
                continue;
            const WL::LandscapeRect whole{ 0u, 0u, data.SamplesX(), data.SamplesZ() };
            if ( auto ok = WL::MergeLandscapeEditLayers( stack, rules, whole, data ); !ok )
                return Common::MakeFormattedError<bool>( "landscape edit layers: tile {}_{}: {}", tile.TileX,
                                                         tile.TileZ, ok.GetError() );
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Commands
