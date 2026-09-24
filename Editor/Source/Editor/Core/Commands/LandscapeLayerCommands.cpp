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
} // namespace Desert::Editor::Commands
