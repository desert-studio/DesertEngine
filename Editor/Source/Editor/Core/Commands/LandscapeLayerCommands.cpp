#include "LandscapeLayerCommands.hpp"
#include "LandscapeEditLayerEdits.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/LandscapeLayerInfoAsset.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/LandscapeEditTarget.hpp>
#include <Engine/ECS/LandscapeLayerRules.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Editor/Core/Selection/LandscapeSculptState.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Editor/Import/LandscapeHeightmapIO.hpp>

#include <Common/Core/Constants.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <span>
#include <utility>

namespace Desert::Editor::Commands
{
    namespace
    {
        class LandscapeLayersCommand final : public ICommand
        {
        public:
            LandscapeLayersCommand( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    const Common::UUID& landscape, std::vector<Assets::AssetHandle> before,
                                    std::vector<Assets::AssetHandle> after, std::string label )
                 : m_Scene( scene ), m_Landscape( landscape ), m_Before( std::move( before ) ),
                   m_After( std::move( after ) ), m_Label( std::move( label ) )
            {
            }

            bool Undo() override
            {
                return Write( m_Before );
            }
            bool Redo() override
            {
                return Write( m_After );
            }
            std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            bool Write( const std::vector<Assets::AssetHandle>& layers )
            {
                const auto scene = m_Scene.lock();
                if ( !scene )
                {
                    ToastManager::Push( "landscape layers: the scene this edit was made in is closed",
                                        ToastLevel::Error, 6.0f );
                    return false;
                }
                auto&      registry = scene->GetRegistry();
                const auto root     = ECS::FindLandscapeRootEntity( registry, m_Landscape );
                if ( root == entt::null )
                {
                    ToastManager::Push( "landscape layers: the landscape root is not loaded", ToastLevel::Error,
                                        6.0f );
                    return false;
                }
                registry.get<ECS::LandscapeComponent>( root ).Layers = layers;
                return true;
            }

            std::weak_ptr<::Desert::Core::Scene> m_Scene;
            Common::UUID                         m_Landscape;
            std::vector<Assets::AssetHandle>     m_Before;
            std::vector<Assets::AssetHandle>     m_After;
            std::string                          m_Label;
        };

        /// Creates (redo) or destroys (undo) a generated landscape: the root and its tiles, by fixed UUIDs.
        class CreateLandscapeCommand final : public ICommand
        {
        public:
            CreateLandscapeCommand( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    World::Landscape::LandscapeGenerated generated, uint32_t quads,
                                    Common::UUID root, std::vector<Common::UUID> tiles )
                 : m_Scene( scene ), m_Generated( std::move( generated ) ), m_Quads( quads ), m_Root( root ),
                   m_Tiles( std::move( tiles ) )
            {
            }

            bool Undo() override
            {
                const auto scene = m_Scene.lock();
                if ( !scene )
                    return false;
                for ( const auto& id : m_Tiles )
                    if ( auto e = scene->FindEntityByID( id ) )
                        scene->DestroyEntity( e->get() );
                if ( auto e = scene->FindEntityByID( m_Root ) )
                    scene->DestroyEntity( e->get() );
                return true;
            }
            bool Redo() override
            {
                const auto scene = m_Scene.lock();
                if ( !scene )
                    return false;
                auto& root = scene->CreateEntityWithUUID( m_Root, "Landscape" );
                if ( !root.HasComponent<ECS::TransformComponent>() )
                    root.AddComponent<ECS::TransformComponent>();
                root.GetComponent<ECS::TransformComponent>().Translation = m_Generated.Root.Origin;
                auto& landscape        = root.AddComponent<ECS::LandscapeComponent>();
                landscape.QuadsPerTile = m_Quads;
                landscape.SpacingCm    = m_Generated.Root.SpacingCm;
                landscape.ZScale       = m_Generated.Root.ZScale;
                landscape.EditLayers   = m_Generated.EditLayers;
                for ( size_t i = 0; i < m_Tiles.size(); ++i )
                {
                    const auto& generated = m_Generated.Tiles[i];
                    auto&       entity    = scene->CreateEntityWithUUID(
                         m_Tiles[i], "Landscape Tile " + std::to_string( generated.TileX ) + "_" +
                                          std::to_string( generated.TileZ ) );
                    auto& tile     = entity.AddComponent<ECS::LandscapeTileComponent>();
                    tile.Landscape = m_Root;
                    tile.TileX     = generated.TileX;
                    tile.TileZ     = generated.TileZ;
                    tile.Heights   = generated.Heights;
                }
                return true;
            }
            std::string GetLabel() const override
            {
                return "New Landscape";
            }

        private:
            std::weak_ptr<::Desert::Core::Scene> m_Scene;
            World::Landscape::LandscapeGenerated m_Generated;
            uint32_t                             m_Quads = 0u;
            Common::UUID                         m_Root;
            std::vector<Common::UUID>            m_Tiles;
        };

        /// Distinct swatches for new layers (UE picks LayerUsageDebugColor per layer; any distinct set serves).
        constexpr std::array<glm::vec3, 6> kLayerSwatches = { {
             { 0.85f, 0.35f, 0.30f },
             { 0.35f, 0.70f, 0.35f },
             { 0.30f, 0.50f, 0.90f },
             { 0.90f, 0.75f, 0.30f },
             { 0.70f, 0.40f, 0.85f },
             { 0.30f, 0.80f, 0.80f },
        } };
    } // namespace

    void RecordLandscapeLayersEdit( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    const Common::UUID& landscape, std::vector<Assets::AssetHandle> before,
                                    std::vector<Assets::AssetHandle> after, std::string label )
    {
        CommandHistory::Get().PushCommand( std::make_unique<LandscapeLayersCommand>(
             scene, landscape, std::move( before ), std::move( after ), std::move( label ) ) );
    }

    namespace
    {
        struct RootRef
        {
            Common::UUID Landscape;
            entt::entity Root = entt::null; // re-read through the registry at each use: the pool may move
        };

        Common::ResultStr<RootRef> RootOf( const std::shared_ptr<::Desert::Core::Scene>& scene )
        {
            if ( !scene )
                return Common::MakeFormattedError<RootRef>( "landscape layers: no scene" );
            auto&      registry  = scene->GetRegistry();
            const auto landscape = ECS::FirstLandscape( registry );
            if ( !landscape )
                return Common::MakeFormattedError<RootRef>( "landscape layers: the scene has no landscape" );
            const auto root = ECS::FindLandscapeRootEntity( registry, *landscape );
            if ( root == entt::null )
                return Common::MakeFormattedError<RootRef>( "landscape layers: the landscape root is not loaded" );
            return Common::MakeSuccess( RootRef{ *landscape, root } );
        }
    } // namespace

    Common::ResultStr<std::string> AddLandscapeLayer( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        auto root = RootOf( scene );
        if ( !root )
            return Common::MakeError<std::string>( root.GetError() );
        auto&       layers = scene->GetRegistry().get<ECS::LandscapeComponent>( root.GetValue().Root ).Layers;
        const auto  before = layers;
        const auto& dir    = Common::Constants::Path::LANDSCAPE_LAYER_INFO_PATH;
        std::string name;
        for ( size_t n = 1;; ++n )
        {
            name = "Layer " + std::to_string( n );
            std::error_code ec;
            if ( !std::filesystem::exists( dir / ( name + Assets::Serialization::kLandscapeLayerInfoExtension ),
                                           ec ) )
                break;
        }
        Assets::Serialization::LandscapeLayerInfoData data;
        data.LayerName            = name;
        data.LayerUsageDebugColor = kLayerSwatches[layers.size() % kLayerSwatches.size()];
        const auto file           = dir / ( name + Assets::Serialization::kLandscapeLayerInfoExtension );
        if ( auto saved = Assets::LandscapeLayerInfoAsset::Save( file, data ); !saved )
            return Common::MakeError<std::string>( saved.GetError() );
        const auto identity = Assets::ReadTextAssetIdentity( file );
        if ( identity.Guid.IsNull() )
            return Common::MakeFormattedError<std::string>( "'{}' was written but states no GUID", file.string() );
        Assets::ContentRegistry::NoteFile( file );
        layers.push_back( identity.Handle() );
        RecordLandscapeLayersEdit( scene, root.GetValue().Landscape, before, layers,
                                   "Create landscape layer " + name );
        return Common::MakeSuccess( name );
    }

    Common::BoolResultStr AssignLandscapeLayer( const std::shared_ptr<::Desert::Core::Scene>& scene, size_t slot,
                                                const Assets::AssetHandle& handle )
    {
        auto root = RootOf( scene );
        if ( !root )
            return Common::MakeError<bool>( root.GetError() );
        auto& layers = scene->GetRegistry().get<ECS::LandscapeComponent>( root.GetValue().Root ).Layers;
        if ( slot >= layers.size() )
            return Common::MakeFormattedError<bool>( "landscape layers: slot {} of {}", slot, layers.size() );
        if ( std::find( layers.begin(), layers.end(), handle ) != layers.end() )
            return Common::MakeFormattedError<bool>( "landscape layers: that layer info is already listed" );
        const auto before = layers;
        layers[slot]      = handle;
        RecordLandscapeLayersEdit( scene, root.GetValue().Landscape, before, layers, "Assign landscape layer" );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr RemoveLandscapeLayer( const std::shared_ptr<::Desert::Core::Scene>& scene, size_t slot )
    {
        auto root = RootOf( scene );
        if ( !root )
            return Common::MakeError<bool>( root.GetError() );
        auto& layers = scene->GetRegistry().get<ECS::LandscapeComponent>( root.GetValue().Root ).Layers;
        if ( slot >= layers.size() )
            return Common::MakeFormattedError<bool>( "landscape layers: slot {} of {}", slot, layers.size() );
        const auto before = layers;
        layers.erase( layers.begin() + static_cast<std::ptrdiff_t>( slot ) );
        RecordLandscapeLayersEdit( scene, root.GetValue().Landscape, before, layers, "Remove landscape layer" );
        return BOOLSUCCESS;
    }

    namespace
    {
        /// The one New Landscape run and the scene it was started for. One run at a time, as UE's Create is
        /// modal; the scene is held weakly, so closing it mid-run is a refusal at the hand-over, not a crash.
        struct CreateLandscapeRun
        {
            World::Landscape::LandscapeGenerateJob Job;
            std::weak_ptr<::Desert::Core::Scene>   Scene;
        };

        CreateLandscapeRun& Run()
        {
            static CreateLandscapeRun run;
            return run;
        }
    } // namespace

    Common::BoolResultStr StartCreateLandscape( const std::shared_ptr<::Desert::Core::Scene>&      scene,
                                                const World::Landscape::LandscapeGenerateSettings& settings )
    {
        if ( !scene )
            return Common::MakeError( "new landscape: no scene" );
        auto started = Run().Job.Start( settings );
        if ( started.IsSuccess() )
            Run().Scene = scene;
        return started;
    }

    bool IsCreatingLandscape()
    {
        return Run().Job.Running();
    }

    float CreateLandscapeFraction()
    {
        return Run().Job.Fraction();
    }

    void CancelCreateLandscape()
    {
        Run().Job.Cancel();
    }

    std::optional<Common::ResultStr<Common::UUID>> FinishCreateLandscape()
    {
        auto finished = Run().Job.TakeFinished();
        if ( !finished )
            return std::nullopt;
        const auto scene = Run().Scene.lock();
        Run().Scene.reset();
        if ( !finished->IsSuccess() )
            return Common::MakeError<Common::UUID>( finished->GetError() );
        if ( !scene )
            return Common::MakeError<Common::UUID>(
                 "new landscape: the scene it was generated for was closed before it finished" );
        auto                      generated = finished->ExtractValue();
        const uint32_t            quads     = generated.Root.QuadsPerTile;
        const Common::UUID        root      = Common::UUID::Generate();
        std::vector<Common::UUID> tiles( generated.Tiles.size() );
        for ( auto& id : tiles )
            id = Common::UUID::Generate();
        auto command = std::make_unique<CreateLandscapeCommand>( scene, std::move( generated ), quads, root,
                                                                 std::move( tiles ) );
        command->Redo();
        CommandHistory::Get().PushCommand( std::move( command ) );
        return Common::MakeSuccess( root );
    }

    namespace
    {
        /// An import into an existing landscape, undone and redone as a whole (UE: one FScopedTransaction).
        class LandscapeHeightsCommand final : public ICommand
        {
        public:
            LandscapeHeightsCommand( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                     const Common::UUID& landscape, World::Landscape::LandscapeStrokeRecord record,
                                     World::Landscape::LandscapeEditLayerTarget layer, std::string label )
                 : m_Scene( scene ), m_Landscape( landscape ), m_Record( std::move( record ) ),
                   m_Layer( std::move( layer ) ), m_Label( std::move( label ) )
            {
            }

            bool Undo() override
            {
                return Write( m_Record.Before );
            }
            bool Redo() override
            {
                return Write( m_Record.After );
            }
            std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            bool Write( const std::vector<uint16_t>& values )
            {
                const auto scene = m_Scene.lock();
                if ( !scene )
                {
                    ToastManager::Push( "heightmap import: the scene it was made in is closed", ToastLevel::Error,
                                        6.0f );
                    return false;
                }
                auto target = ECS::FindLandscapeEditTarget( scene->GetRegistry(), m_Landscape );
                if ( !target.IsSuccess() )
                {
                    ToastManager::Push( target.GetError(), ToastLevel::Error, 6.0f );
                    return false;
                }
                const auto& t = target.GetValue();
                auto        written =
                     World::Landscape::WriteLandscapeHeights( t.Root, t.Lookup, m_Record.Rect, values, m_Layer );
                if ( !written.IsSuccess() )
                    ToastManager::Push( written.GetError(), ToastLevel::Error, 6.0f );
                return written.IsSuccess();
            }

            std::weak_ptr<::Desert::Core::Scene>    m_Scene;
            Common::UUID                            m_Landscape;
            World::Landscape::LandscapeStrokeRecord    m_Record;
            World::Landscape::LandscapeEditLayerTarget m_Layer;
            std::string                                m_Label;
        };

        Common::ResultStr<ECS::LandscapeEditTarget>
        HeightmapTarget( const std::shared_ptr<::Desert::Core::Scene>& scene )
        {
            auto root = RootOf( scene );
            if ( !root )
                return Common::MakeError<ECS::LandscapeEditTarget>( "heightmap: " + root.GetError() );
            return ECS::FindLandscapeEditTarget( scene->GetRegistry(), root.GetValue().Landscape );
        }

        /// The layer an import writes: the one the Landscape panel edits (LandscapeSculptState::EditingLayer),
        /// the bottom one when none is picked — UE's Import writes the editing layer.
        Common::ResultStr<World::Landscape::LandscapeEditLayerTarget>
        HeightmapLayer( const std::shared_ptr<::Desert::Core::Scene>& scene, const Common::UUID& landscape )
        {
            auto&      registry = scene->GetRegistry();
            const auto root     = ECS::FindLandscapeRootEntity( registry, landscape );
            if ( root == entt::null )
                return Common::MakeFormattedError<World::Landscape::LandscapeEditLayerTarget>(
                     "heightmap: the landscape root is not loaded" );
            auto rules = ECS::LandscapeLayerRulesOf( registry.get<ECS::LandscapeComponent>( root ),
                                                     *Runtime::ResourceRegistry::GetLandscapeLayerInfoService() );
            if ( !rules )
                return Common::MakeFormattedError<World::Landscape::LandscapeEditLayerTarget>( "heightmap: {}",
                                                                                               rules.GetError() );
            return ECS::FindLandscapeEditLayerTarget( registry, landscape, rules.ExtractValue(),
                                                      Core::LandscapeSculptState::Get().EditingLayer );
        }
    } // namespace

    Common::BoolResultStr ImportLandscapeHeightmap( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                    const std::filesystem::path&                  path )
    {
        auto target = HeightmapTarget( scene );
        if ( !target )
            return Common::MakeError<bool>( target.GetError() );
        const auto& t = target.GetValue();
        const auto  size =
             World::Landscape::LandscapeHeightmapSize{ static_cast<uint32_t>( t.Bounds.X2 - t.Bounds.X1 + 1 ),
                                                       static_cast<uint32_t>( t.Bounds.Z2 - t.Bounds.Z1 + 1 ) };
        auto map = World::Landscape::ReadLandscapeHeightmapFile( path, size );
        if ( !map )
            return Common::MakeError<bool>( map.GetError() );
        auto layer = HeightmapLayer( scene, t.Landscape );
        if ( !layer )
            return Common::MakeError<bool>( layer.GetError() );
        auto record = World::Landscape::ImportLandscapeHeightmap( t.Root, t.Lookup, t.Bounds, map.GetValue(),
                                                                  layer.GetValue() );
        if ( !record )
            return Common::MakeFormattedError<bool>( "{}: {}", path.generic_string(), record.GetError() );
        CommandHistory::Get().PushCommand( std::make_unique<LandscapeHeightsCommand>(
             scene, t.Landscape, record.ExtractValue(), layer.ExtractValue(),
             "Import heightmap " + path.filename().string() ) );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<Common::UUID>
    ImportLandscapeHeightmapAsNew( const std::shared_ptr<::Desert::Core::Scene>&      scene,
                                   const std::filesystem::path&                       path,
                                   const World::Landscape::LandscapeGenerateSettings& settings )
    {
        if ( !scene )
            return Common::MakeError<Common::UUID>( "heightmap new landscape: no scene" );
        if ( IsCreatingLandscape() )
            return Common::MakeFormattedError<Common::UUID>(
                 "heightmap new landscape: a New Landscape Create is running ({:.0f} %)",
                 CreateLandscapeFraction() * 100.0f );
        auto map = World::Landscape::ReadLandscapeHeightmapFile( path, std::nullopt );
        if ( !map )
            return Common::MakeError<Common::UUID>( map.GetError() );
        auto made = World::Landscape::LandscapeFromHeightmap( settings, map.GetValue() );
        if ( !made )
            return Common::MakeFormattedError<Common::UUID>( "{}: {}", path.generic_string(), made.GetError() );
        auto                      generated = made.ExtractValue();
        const uint32_t            quads     = generated.Root.QuadsPerTile;
        const Common::UUID        root      = Common::UUID::Generate();
        std::vector<Common::UUID> tiles( generated.Tiles.size() );
        for ( auto& id : tiles )
            id = Common::UUID::Generate();
        auto command = std::make_unique<CreateLandscapeCommand>( scene, std::move( generated ), quads, root,
                                                                 std::move( tiles ) );
        command->Redo();
        CommandHistory::Get().PushCommand( std::move( command ) );
        return Common::MakeSuccess( root );
    }

    Common::BoolResultStr ExportLandscapeHeightmap( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                    const std::filesystem::path& path, bool selectedTiles )
    {
        auto target = HeightmapTarget( scene );
        if ( !target )
            return Common::MakeError<bool>( target.GetError() );
        const auto&                             t    = target.GetValue();
        World::Landscape::LandscapeSampleBounds rect = t.Bounds;
        if ( selectedTiles )
        {
            bool    any = false;
            int32_t x1 = 0, z1 = 0, x2 = 0, z2 = 0;
            for ( const auto& id : Core::SelectionManager::GetSelection() )
            {
                const auto entity = scene->FindEntityByID( id );
                if ( !entity || !entity->get().HasComponent<ECS::LandscapeTileComponent>() )
                    continue;
                const auto& tile = entity->get().GetComponent<ECS::LandscapeTileComponent>();
                if ( tile.Landscape != t.Landscape )
                    continue;
                x1  = any ? std::min( x1, tile.TileX ) : tile.TileX;
                z1  = any ? std::min( z1, tile.TileZ ) : tile.TileZ;
                x2  = any ? std::max( x2, tile.TileX ) : tile.TileX;
                z2  = any ? std::max( z2, tile.TileZ ) : tile.TileZ;
                any = true;
            }
            if ( !any )
                return Common::MakeError<bool>( "heightmap export: no tile of the landscape is selected; select "
                                                "tiles in the Outliner first" );
            rect = World::Landscape::LandscapeTileRangeSamples( t.Root, x1, z1, x2, z2 );
        }
        auto map = World::Landscape::ReadLandscapeHeightmap( t.Root, t.Lookup, rect );
        if ( !map )
            return Common::MakeFormattedError<bool>( "{}: {}", path.generic_string(), map.GetError() );
        return World::Landscape::WriteLandscapeHeightmapFile( path, map.GetValue() );
    }
    namespace
    {
        namespace WL = World::Landscape;

        /// The paint rules the merge reads — none needed while the landscape has no target layers.
        Common::ResultStr<std::vector<WL::LandscapeLayerRule>> MergeRules( entt::registry&     registry,
                                                                           const Common::UUID& landscape )
        {
            using Rules     = std::vector<WL::LandscapeLayerRule>;
            const auto root = ECS::FindLandscapeRootEntity( registry, landscape );
            if ( root == entt::null )
                return Common::MakeError<Rules>( "landscape edit layers: the landscape root is not loaded" );
            const auto& body = registry.get<ECS::LandscapeComponent>( root );
            if ( body.Layers.empty() )
                return Common::MakeSuccess( Rules{} );
            auto* service = Runtime::ResourceRegistry::GetLandscapeLayerInfoService();
            if ( service == nullptr )
                return Common::MakeError<Rules>( "landscape edit layers: no landscape layer info service" );
            auto rules = ECS::LandscapeLayerRulesOf( body, *service );
            if ( !rules )
                return Common::MakeFormattedError<Rules>( "landscape edit layers: {}", rules.GetError() );
            return rules;
        }

        /// Applies @p stack (with @p restore) and points the editing layer back at the bottom one when @p stack
        /// no longer names it.
        Common::BoolResultStr ApplyEditLayers( entt::registry& registry, const Common::UUID& landscape,
                                               const WL::LandscapeEditLayerStack&         stack,
                                               std::span<const LandscapeRemovedTileLayer> restore, bool merge )
        {
            std::vector<WL::LandscapeLayerRule> rules;
            if ( merge )
            {
                auto read = MergeRules( registry, landscape );
                if ( !read )
                    return Common::MakeError<bool>( read.GetError() );
                rules = read.ExtractValue();
            }
            if ( auto ok = ApplyLandscapeEditLayerStack( registry, landscape, stack, rules, restore, merge ); !ok )
                return ok;
            auto& editing = Core::LandscapeSculptState::Get().EditingLayer;
            if ( !editing.IsNull() && stack.Find( editing ) == nullptr )
                editing = Common::UUID::Null();
            return BOOLSUCCESS;
        }

        class LandscapeEditLayersCommand final : public ICommand
        {
        public:
            LandscapeEditLayersCommand( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                        const Common::UUID& landscape, WL::LandscapeEditLayerStack before,
                                        WL::LandscapeEditLayerStack            after,
                                        std::vector<LandscapeRemovedTileLayer> removed, bool merge,
                                        std::string label )
                 : m_Scene( scene ), m_Landscape( landscape ), m_Before( std::move( before ) ),
                   m_After( std::move( after ) ), m_Removed( std::move( removed ) ), m_Merge( merge ),
                   m_Label( std::move( label ) )
            {
            }

            bool Undo() override
            {
                return Write( m_Before, m_Removed );
            }
            bool Redo() override
            {
                return Write( m_After, {} );
            }
            std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            bool Write( const WL::LandscapeEditLayerStack&         stack,
                        std::span<const LandscapeRemovedTileLayer> restore )
            {
                const auto scene = m_Scene.lock();
                if ( !scene )
                {
                    ToastManager::Push( "landscape edit layers: the scene this edit was made in is closed",
                                        ToastLevel::Error, 6.0f );
                    return false;
                }
                if ( auto ok = ApplyEditLayers( scene->GetRegistry(), m_Landscape, stack, restore, m_Merge ); !ok )
                {
                    ToastManager::Push( ok.GetError(), ToastLevel::Error, 6.0f );
                    return false;
                }
                return true;
            }

            std::weak_ptr<::Desert::Core::Scene>   m_Scene;
            Common::UUID                           m_Landscape;
            WL::LandscapeEditLayerStack            m_Before;
            WL::LandscapeEditLayerStack            m_After;
            std::vector<LandscapeRemovedTileLayer> m_Removed;
            bool                                   m_Merge = false;
            std::string                            m_Label;
        };

        /// The first landscape, its stack, and @p layer's index in it (0 when @p layer is null).
        struct StackEdit
        {
            Common::UUID                Landscape;
            WL::LandscapeEditLayerStack Stack;
            size_t                      Index = 0;
        };

        Common::ResultStr<StackEdit> StackOf( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                              const Common::UUID&                           layer )
        {
            auto root = RootOf( scene );
            if ( !root )
                return Common::MakeError<StackEdit>( root.GetError() );
            StackEdit edit{ root.GetValue().Landscape,
                            scene->GetRegistry().get<ECS::LandscapeComponent>( root.GetValue().Root ).EditLayers };
            if ( layer.IsNull() )
                return Common::MakeSuccess( std::move( edit ) );
            auto index = LandscapeEditLayerIndex( edit.Stack, layer );
            if ( !index )
                return Common::MakeError<StackEdit>( index.GetError() );
            edit.Index = index.GetValue();
            return Common::MakeSuccess( std::move( edit ) );
        }

        /// Applies @p after and records the edit as ONE undo entry.
        Common::BoolResultStr CommitEditLayers( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                const StackEdit& edit, WL::LandscapeEditLayerStack after,
                                                std::vector<LandscapeRemovedTileLayer> removed, bool merge,
                                                std::string label )
        {
            if ( auto ok = ApplyEditLayers( scene->GetRegistry(), edit.Landscape, after, {}, merge ); !ok )
                return ok;
            CommandHistory::Get().PushCommand( std::make_unique<LandscapeEditLayersCommand>(
                 scene, edit.Landscape, edit.Stack, std::move( after ), std::move( removed ), merge,
                 std::move( label ) ) );
            return BOOLSUCCESS;
        }

        /// One field of layer @p layer, edited through @p edit, as one undo entry.
        template <typename Edit>
        Common::BoolResultStr EditLayerField( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                              const Common::UUID& layer, bool merge, std::string label,
                                              Edit&& edit )
        {
            auto stack = StackOf( scene, layer );
            if ( !stack )
                return Common::MakeError<bool>( stack.GetError() );
            auto after = stack.GetValue().Stack;
            edit( after.Layers[stack.GetValue().Index] );
            return CommitEditLayers( scene, stack.GetValue(), std::move( after ), {}, merge, std::move( label ) );
        }
    } // namespace

    Common::ResultStr<Common::UUID> AddLandscapeEditLayer( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        auto edit = StackOf( scene, Common::UUID::Null() );
        if ( !edit )
            return Common::MakeError<Common::UUID>( edit.GetError() );
        const auto guid  = Common::UUID::Generate();
        auto       after = LandscapeStackWithLayerAdded( edit.GetValue().Stack,
                                                         Core::LandscapeSculptState::Get().EditingLayer, guid );
        if ( auto ok = CommitEditLayers( scene, edit.GetValue(), std::move( after ), {}, false,
                                         "Add landscape edit layer" );
             !ok )
            return Common::MakeError<Common::UUID>( ok.GetError() );
        Core::LandscapeSculptState::Get().EditingLayer = guid;
        return Common::MakeSuccess( guid );
    }

    Common::BoolResultStr RemoveLandscapeEditLayer( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                    const Common::UUID&                           layer )
    {
        auto edit = StackOf( scene, layer );
        if ( !edit )
            return Common::MakeError<bool>( edit.GetError() );
        auto after = LandscapeStackWithLayerRemoved( edit.GetValue().Stack, layer );
        if ( !after )
            return Common::MakeError<bool>( after.GetError() );
        auto removed = LandscapeEditLayerTileDataOf( scene->GetRegistry(), edit.GetValue().Landscape, layer );
        if ( !removed )
            return Common::MakeError<bool>( removed.GetError() );
        return CommitEditLayers( scene, edit.GetValue(), after.ExtractValue(), removed.ExtractValue(), true,
                                 "Delete landscape edit layer" );
    }

    Common::BoolResultStr RenameLandscapeEditLayer( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                    const Common::UUID& layer, std::string name )
    {
        return EditLayerField( scene, layer, false, "Rename landscape edit layer",
                               [&]( WL::LandscapeEditLayer& l ) { l.Name = std::move( name ); } );
    }

    Common::BoolResultStr SetLandscapeEditLayerVisible( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                        const Common::UUID& layer, bool visible )
    {
        return EditLayerField( scene, layer, true,
                               visible ? "Show landscape edit layer" : "Hide landscape edit layer",
                               [&]( WL::LandscapeEditLayer& l ) { l.Visible = visible; } );
    }

    Common::BoolResultStr SetLandscapeEditLayerLocked( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                       const Common::UUID& layer, bool locked )
    {
        return EditLayerField( scene, layer, false,
                               locked ? "Lock landscape edit layer" : "Unlock landscape edit layer",
                               [&]( WL::LandscapeEditLayer& l ) { l.Locked = locked; } );
    }

    Common::BoolResultStr SetLandscapeEditLayerAlpha( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                      const Common::UUID& layer, float heightAlpha,
                                                      float weightAlpha )
    {
        return EditLayerField( scene, layer, true, "Landscape edit layer alpha",
                               [&]( WL::LandscapeEditLayer& l )
                               {
                                   l.HeightAlpha = heightAlpha;
                                   l.WeightAlpha = weightAlpha;
                               } );
    }
} // namespace Desert::Editor::Commands
