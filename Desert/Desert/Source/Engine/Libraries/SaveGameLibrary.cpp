#include <Engine/Libraries/SaveGameLibrary.hpp>

#include <Engine/Core/SaveGame.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/Reflection/FunctionThunk.hpp>

#include <format>

namespace Desert::Libraries
{
    namespace
    {
        const Core::WorldContext* Played()
        {
            const Core::WorldContext* context = Core::WorldContext::Current();
            return context != nullptr && context->World != nullptr ? context : nullptr;
        }
    } // namespace

    Common::BoolResultStr SaveGameLibrary::Save( const std::string& slot, std::uint32_t user )
    {
        const Core::WorldContext* world = Played();
        if ( world == nullptr )
            return Common::MakeError<bool>( "savegame.save: no scene is running" );
        return Core::SaveGameToSlot( *world->World, slot, user );
    }

    Common::ResultStr<std::vector<std::string>> SaveGameLibrary::Load( const std::string& slot,
                                                                       std::uint32_t      user )
    {
        using Skipped                   = std::vector<std::string>;
        const Core::WorldContext* world = Played();
        if ( world == nullptr )
            return Common::MakeError<Skipped>( "savegame.load: no scene is running" );
        Common::ResultStr<Core::SaveGameLoadReport> loaded = Core::LoadGameFromSlot( *world->World, slot, user );
        if ( !loaded )
            return Common::MakeError<Skipped>( loaded.GetError() );
        // The load wrote the SaveGame properties into the script slots; the running scripts get them NOW, so the
        // caller reads the loaded values on its next line and the read-back after this OnUpdate does not copy the
        // pre-load values over them.
        if ( world->Host != nullptr )
            world->Host->ApplySaveGameProperties();
        return Common::MakeSuccess( loaded.ExtractValue().Problems );
    }

    bool SaveGameLibrary::Exists( const std::string& slot, std::uint32_t user )
    {
        // A failure here is no save root (no project): a configuration error, not "no such slot".
        const Common::ResultStr<bool> exists = Core::DoesSaveGameExist( slot, user );
        if ( !exists )
        {
            Reflection::RaiseCallError( std::format( "savegame.exists('{}'): {}", slot, exists.GetError() ) );
            return false;
        }
        return exists.GetValue();
    }

    Common::BoolResultStr SaveGameLibrary::Delete( const std::string& slot, std::uint32_t user )
    {
        return Core::DeleteGameInSlot( slot, user );
    }

    Common::ResultStr<std::vector<std::string>> SaveGameLibrary::List( std::uint32_t user )
    {
        return Core::ListSaveGameSlots( user );
    }

    Common::ResultStr<Reflection::Value::Map> SaveGameLibrary::Scene( const std::string& slot, std::uint32_t user )
    {
        using Map       = Reflection::Value::Map;
        const auto root = Core::SaveGameRoot();
        if ( !root )
            return Common::MakeError<Map>( root.GetError() );
        const auto document = Core::ReadSaveGameSlot( root.GetValue(), slot, user );
        if ( !document )
            return Common::MakeError<Map>( document.GetError() );
        const auto scene = Core::SaveGameSceneOf( document.GetValue() );
        if ( !scene )
            return Common::MakeError<Map>( scene.GetError() );
        Map identity;
        identity.Set( "guid", Reflection::Value::String( scene.GetValue().Guid ) );
        identity.Set( "name", Reflection::Value::String( scene.GetValue().Name ) );
        return Common::MakeSuccess( std::move( identity ) );
    }
} // namespace Desert::Libraries
