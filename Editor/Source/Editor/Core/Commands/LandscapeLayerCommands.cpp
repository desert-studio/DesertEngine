#include "LandscapeLayerCommands.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/LandscapeEditTarget.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>

#include <array>
#include <utility>

namespace Desert::Editor::Commands
{
    namespace
    {
        class LandscapeLayersCommand final : public ICommand
        {
        public:
            LandscapeLayersCommand( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    const Common::UUID& landscape, std::vector<ECS::LandscapeLayerInfo> before,
                                    std::vector<ECS::LandscapeLayerInfo> after, std::string label )
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
            bool Write( const std::vector<ECS::LandscapeLayerInfo>& layers )
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
            std::vector<ECS::LandscapeLayerInfo> m_Before;
            std::vector<ECS::LandscapeLayerInfo> m_After;
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

    bool SameLandscapeLayers( const std::vector<ECS::LandscapeLayerInfo>& a,
                              const std::vector<ECS::LandscapeLayerInfo>& b )
    {
        if ( a.size() != b.size() )
            return false;
        for ( size_t i = 0; i < a.size(); ++i )
        {
            if ( a[i].Name != b[i].Name || a[i].Hardness != b[i].Hardness ||
                 a[i].NoWeightBlend != b[i].NoWeightBlend || a[i].Color != b[i].Color )
                return false;
        }
        return true;
    }

    void RecordLandscapeLayersEdit( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    const Common::UUID& landscape, std::vector<ECS::LandscapeLayerInfo> before,
                                    std::vector<ECS::LandscapeLayerInfo> after, std::string label )
    {
        CommandHistory::Get().PushCommand( std::make_unique<LandscapeLayersCommand>(
             scene, landscape, std::move( before ), std::move( after ), std::move( label ) ) );
    }

    Common::ResultStr<std::string> AddLandscapeLayer( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        if ( !scene )
            return Common::MakeFormattedError<std::string>( "landscape layers: no scene" );
        auto&      registry  = scene->GetRegistry();
        const auto landscape = ECS::FirstLandscape( registry );
        if ( !landscape )
            return Common::MakeFormattedError<std::string>( "landscape layers: the scene has no landscape" );
        const auto root = ECS::FindLandscapeRootEntity( registry, *landscape );
        if ( root == entt::null )
            return Common::MakeFormattedError<std::string>( "landscape layers: the landscape root is not loaded" );
        auto&       layers = registry.get<ECS::LandscapeComponent>( root ).Layers;
        const auto  before = layers;
        std::string name;
        for ( size_t n = 1;; ++n )
        {
            name       = "Layer " + std::to_string( n );
            bool taken = false;
            for ( const auto& layer : layers )
                taken = taken || layer.Name == name;
            if ( !taken )
                break;
        }
        ECS::LandscapeLayerInfo added;
        added.Name  = name;
        added.Color = kLayerSwatches[layers.size() % kLayerSwatches.size()];
        layers.push_back( added );
        RecordLandscapeLayersEdit( scene, *landscape, before, layers, "Add landscape layer " + name );
        return Common::MakeSuccess( name );
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
} // namespace Desert::Editor::Commands
