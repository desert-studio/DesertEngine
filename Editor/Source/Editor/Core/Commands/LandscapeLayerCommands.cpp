#include "LandscapeLayerCommands.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/LandscapeLayerInfoAsset.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/LandscapeEditTarget.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>

#include <Common/Core/Constants.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
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
            Common::UUID                      Landscape;
            std::vector<Assets::AssetHandle>* Layers = nullptr;
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
            return Common::MakeSuccess(
                 RootRef{ *landscape, &registry.get<ECS::LandscapeComponent>( root ).Layers } );
        }
    } // namespace

    Common::ResultStr<std::string> AddLandscapeLayer( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        auto root = RootOf( scene );
        if ( !root )
            return Common::MakeError<std::string>( root.GetError() );
        auto&       layers = *root.GetValue().Layers;
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
        auto& layers = *root.GetValue().Layers;
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
        auto& layers = *root.GetValue().Layers;
        if ( slot >= layers.size() )
            return Common::MakeFormattedError<bool>( "landscape layers: slot {} of {}", slot, layers.size() );
        const auto before = layers;
        layers.erase( layers.begin() + static_cast<std::ptrdiff_t>( slot ) );
        RecordLandscapeLayersEdit( scene, root.GetValue().Landscape, before, layers, "Remove landscape layer" );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Commands
