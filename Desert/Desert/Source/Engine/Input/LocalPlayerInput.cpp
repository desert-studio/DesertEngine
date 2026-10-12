#include <Engine/Input/LocalPlayerInput.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/EnhancedInputAssets.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Core/Input.hpp>
#include <Engine/ECS/Components.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <filesystem>

namespace Desert::Input
{
    using namespace Assets::Serialization;

    namespace
    {
        std::string StemOf( const std::filesystem::path& path )
        {
            return path.stem().string();
        }

        // The context asset's data and every action it maps, read through the loader.
        Common::BoolResultStr
        ReadContextAndActions( Assets::AssetManager&                                  assets,
                               const Assets::Asset<Assets::InputMappingContextAsset>& context,
                               std::map<std::string, InputActionData>&                actions )
        {
            if ( auto read = Assets::LoadThroughLoader( assets, context ); !read )
                return read;
            for ( const InputKeyMappingData& mapping : context->GetData().Mappings )
            {
                const auto guid = Common::Content::AssetGuidFromText( mapping.Action.Guid );
                if ( !guid )
                    return Common::MakeFormattedError<bool>( "action GUID '{}' is not an asset GUID",
                                                             mapping.Action.Guid );
                const auto action = Assets::CreateFromRegistryGuid<Assets::InputActionAsset>(
                     assets, guid.GetValue(), Common::Content::ContentKind::InputAction );
                if ( !action )
                    return Common::MakeFormattedError<bool>(
                         "action '{}' ({}) is not an input action of this project", mapping.Action.Guid,
                         mapping.Action.Path );
                if ( auto read = Assets::LoadThroughLoader( assets, action ); !read )
                    return read;
                actions[Common::Content::AssetGuidToText( guid.GetValue() )] = action->GetData();
            }
            return BOOLSUCCESS;
        }
    } // namespace

    RawInputFrame SampleRawInput( const std::vector<InputKey>& keys, const glm::vec2 mouseDelta,
                                  const std::function<bool( Common::KeyCode )>&     keyDown,
                                  const std::function<bool( Common::MouseButton )>& buttonDown )
    {
        RawInputFrame frame;
        frame.MouseDelta = mouseDelta;
        for ( const InputKey& key : keys )
        {
            if ( key.Kind == InputKey::Device::Keyboard )
            {
                const auto code = static_cast<Common::KeyCode>( key.Code );
                if ( keyDown( code ) )
                    frame.KeysDown.push_back( code );
            }
            else if ( key.Kind == InputKey::Device::MouseButton )
            {
                const auto button = static_cast<Common::MouseButton>( key.Code );
                if ( buttonDown( button ) )
                    frame.MouseButtonsDown.push_back( button );
            }
        }
        return frame;
    }

    std::vector<std::pair<Assets::AssetHandle, int>>
    PlayerContextPriorities( const ECS::EnhancedInputPlayerData& player )
    {
        std::vector<std::pair<Assets::AssetHandle, int>> out;
        const int                                        count = static_cast<int>( player.Contexts.size() );
        for ( int i = 0; i < count; ++i )
            out.emplace_back( player.Contexts[i], player.BasePriority + ( count - 1 - i ) );
        return out;
    }

    void LocalPlayerInput::BeginPlay( entt::registry& registry, Assets::AssetManager& assets,
                                      const entt::entity pawn )
    {
        EndPlay();
        if ( const std::filesystem::path file = UserKeyBindingsFile(); !file.empty() )
        {
            if ( auto bindings = LoadUserKeyBindings( file ); bindings )
                m_Subsystem.SetUserKeyBindings( bindings.GetValue() );
            else
                LOG_ERROR( "[Input] {}; the assets' keys are used", bindings.GetError() );
        }

        for ( const auto entity : registry.view<ECS::EnhancedInputPlayerComponent>() )
            AddPlayerContexts( assets, registry.get<ECS::EnhancedInputPlayerComponent>( entity ).Data,
                               entity == pawn ? &m_PawnContexts : nullptr );
    }

    void LocalPlayerInput::PossessPawn( entt::registry& registry, Assets::AssetManager& assets,
                                        const entt::entity pawn )
    {
        UnpossessPawn();
        if ( pawn == entt::null || !registry.valid( pawn ) )
            return;
        if ( const auto* player = registry.try_get<ECS::EnhancedInputPlayerComponent>( pawn ) )
            AddPlayerContexts( assets, player->Data, &m_PawnContexts );
    }

    void LocalPlayerInput::UnpossessPawn()
    {
        for ( const std::string& name : m_PawnContexts )
            if ( !RemoveContext( name ) )
                LOG_ERROR( "[Input] the possessed pawn's mapping context '{}' was no longer active", name );
        m_PawnContexts.clear();
    }

    void LocalPlayerInput::AddPlayerContexts( Assets::AssetManager&               assets,
                                              const ECS::EnhancedInputPlayerData& player,
                                              std::vector<std::string>*           added )
    {
        {
            for ( const auto& [handle, priority] : PlayerContextPriorities( player ) )
            {
                const auto context = assets.FindByHandle<Assets::InputMappingContextAsset>( handle );
                if ( !context )
                {
                    LOG_ERROR( "[Input] the player names mapping context handle {} that no loaded asset carries",
                               static_cast<uint64_t>( handle ) );
                    continue;
                }
                std::map<std::string, InputActionData> actions;
                const std::string                      name = StemOf( context->GetMetadata().Filepath );
                Common::BoolResultStr                  ok   = ReadContextAndActions( assets, context, actions );
                if ( ok )
                    ok = AddLoadedContext( name, context->GetData(), actions, priority );
                if ( !ok )
                {
                    LOG_ERROR( "[Input] mapping context '{}' was not added: {}",
                               context->GetMetadata().Filepath.string(), ok.GetError() );
                }
                else if ( added != nullptr )
                    added->push_back( name );
            }
        }
    }

    void LocalPlayerInput::EndPlay()
    {
        m_Subsystem = EnhancedInputSubsystem{};
        m_ActionNames.clear();
        m_ContextNames.clear();
        m_PawnContexts.clear();
    }

    void LocalPlayerInput::Tick( const glm::vec2 mouseDelta, const float deltaSeconds )
    {
        const RawInputFrame frame = SampleRawInput(
             m_Subsystem.MappedKeys(), mouseDelta, []( Common::KeyCode k ) { return Keyboard::IsKeyPressed( k ); },
             []( Common::MouseButton b ) { return Mouse::Get().IsMouseButtonPressed( b ); } );
        m_Subsystem.Tick( frame, deltaSeconds );
    }

    Common::BoolResultStr LocalPlayerInput::AddContext( Assets::AssetManager& assets, const std::string& name,
                                                        const int priority )
    {
        for ( const auto& row :
              Assets::ContentRegistry::Rows( Common::Content::ContentKind::InputMappingContext ) )
        {
            const std::filesystem::path path( row.Path );
            if ( StemOf( path ) != name && path.generic_string() != name )
                continue;
            const auto context =
                 assets.CreateAsset<Assets::InputMappingContextAsset>( path, /*loadAfterCreate=*/false );
            if ( !context )
                return Common::MakeFormattedError<bool>( "mapping context '{}' could not be created", name );
            std::map<std::string, InputActionData> actions;
            if ( auto read = ReadContextAndActions( assets, context, actions ); !read )
                return read;
            return AddLoadedContext( StemOf( path ), context->GetData(), actions, priority );
        }
        return Common::MakeFormattedError<bool>( "no mapping context of this project is named '{}'", name );
    }

    bool LocalPlayerInput::RemoveContext( const std::string& name )
    {
        const auto it = m_ContextNames.find( name );
        if ( it == m_ContextNames.end() || !m_Subsystem.RemoveMappingContext( it->second ) )
            return false;
        m_ContextNames.erase( it );
        return true;
    }

    Common::BoolResultStr
    LocalPlayerInput::AddLoadedContext( const std::string& name, const InputMappingContextData& context,
                                        const std::map<std::string, InputActionData>& actionsByGuidText,
                                        const int                                     priority )
    {
        for ( const InputKeyMappingData& mapping : context.Mappings )
        {
            const auto guid = Common::Content::AssetGuidFromText( mapping.Action.Guid );
            if ( !guid )
                return Common::MakeFormattedError<bool>( "action GUID '{}' is not an asset GUID",
                                                         mapping.Action.Guid );
            const std::string text  = Common::Content::AssetGuidToText( guid.GetValue() );
            const auto        found = actionsByGuidText.find( text );
            if ( found == actionsByGuidText.end() )
                return Common::MakeFormattedError<bool>(
                     "context '{}' maps action {} ({}), whose data was not read", name, text,
                     mapping.Action.Path );
            m_Subsystem.RegisterAction( guid.GetValue(), found->second );
            m_ActionNames[StemOf( mapping.Action.Path )] = guid.GetValue();
        }
        if ( auto added = m_Subsystem.AddMappingContext( context, priority ); !added )
            return added;
        const auto contextGuid = Common::Content::AssetGuidFromText( context.Header->Guid );
        m_ContextNames[name]   = contextGuid.GetValue();
        return BOOLSUCCESS;
    }

    std::optional<Common::Content::AssetGuid> LocalPlayerInput::ActionNamed( const std::string& name ) const
    {
        const auto it = m_ActionNames.find( name );
        return it != m_ActionNames.end() ? std::optional<Common::Content::AssetGuid>( it->second ) : std::nullopt;
    }

    std::optional<Common::Content::AssetGuid> LocalPlayerInput::ContextNamed( const std::string& name ) const
    {
        const auto it = m_ContextNames.find( name );
        return it != m_ContextNames.end() ? std::optional<Common::Content::AssetGuid>( it->second ) : std::nullopt;
    }

    Common::BoolResultStr LocalPlayerInput::RebindKey( const std::string& context, const std::string& action,
                                                       const std::string& defaultKey, const std::string& key )
    {
        const auto contextGuid = ContextNamed( context );
        if ( !contextGuid )
            return Common::MakeFormattedError<bool>( "no active mapping context is named '{}'", context );
        const auto actionGuid = ActionNamed( action );
        if ( !actionGuid )
            return Common::MakeFormattedError<bool>( "no mapped action is named '{}'", action );
        if ( auto remapped = m_Subsystem.RemapKey( *contextGuid, *actionGuid, defaultKey, key ); !remapped )
            return remapped;
        const std::filesystem::path file = UserKeyBindingsFile();
        if ( file.empty() )
            return Common::MakeFormattedError<bool>(
                 "the binding applies but cannot be saved: this host loaded no user settings directory" );
        return SaveUserKeyBindings( file, m_Subsystem.GetUserKeyBindings() );
    }
} // namespace Desert::Input
