#include "SaveGame.hpp"

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/ReflectedComponentBlocks.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Project/ProjectContext.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <format>
#include <mutex>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Core
{
    namespace
    {
        namespace Json = Common::Json;

        std::mutex& RootMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
        std::filesystem::path& DeclaredRoot()
        {
            static std::filesystem::path root;
            return root;
        }

        // The block's reflected struct inside its component: a member (Camera's Data) or the whole component.
        template <class Row, class Component>
        auto* BlockData( const Row& row, Component& component )
        {
            using Whole = Serialize::ReflectedWholeBlock<typename Row::Component>;
            if constexpr ( std::is_same_v<Row, Whole> )
                return &component;
            else
                return &( component.*row.Member );
        }

        // {"<Key>": {"Type": "<reflected type>", "Fields": {"<field>": {"Type": "<C++ type>", "Value": v}}}} —
        // only the fields carrying PROPERTY(SaveGame); a block with none of them is not written at all.
        Json::Object CaptureBlocks( const entt::registry& registry, entt::entity entity,
                                    const ReflectedTypeLookup& types )
        {
            Json::Object blocks;
            Serialize::ForEachReflectedComponentBlock(
                 [&]( const auto& row )
                 {
                     using Row       = std::remove_cvref_t<decltype( row )>;
                     using Component = typename Row::Component;
                     if ( !registry.template has<Component>( entity ) )
                         return;
                     const Reflection::TypeInfo* type = types( row.TypeName );
                     if ( type == nullptr )
                         return;
                     const bool anyFlagged =
                          std::any_of( type->Fields.begin(), type->Fields.end(),
                                       []( const Reflection::FieldInfo& f ) { return f.Meta.SaveGame; } );
                     if ( !anyFlagged )
                         return;
                     const Component&   component = registry.template get<Component>( entity );
                     const Json::Object whole =
                          Reflection::SerializeReflected( *type, BlockData( row, component ) );
                     Json::Object fields;
                     for ( const Reflection::FieldInfo& field : type->Fields )
                     {
                         if ( !field.Meta.SaveGame )
                             continue;
                         for ( const auto& [name, value] : whole )
                             if ( name == field.Name )
                                 fields[field.Name] = Json::Value( Json::ObjectBuilder{}
                                                                        .Set( "Type", field.TypeName )
                                                                        .Set( "Value", value )
                                                                        .Build() );
                     }
                     blocks[row.Key] = Json::Value(
                          Json::ObjectBuilder{}.Set( "Type", type->Name ).Set( "Fields", fields ).Build() );
                 } );
            return blocks;
        }

        bool ReadVec3( const Json::Node& node, glm::vec3& out )
        {
            if ( node.GetKind() != Json::Kind::Array )
                return false;
            glm::vec3   read( 0.0f );
            std::size_t count = 0;
            bool        valid = true;
            node.ForEachElement(
                 [&]( std::size_t index, const Json::Node& element )
                 {
                     const auto number = element.AsNumber();
                     if ( index >= 3 || !number )
                     {
                         valid = false;
                         return;
                     }
                     read[static_cast<glm::length_t>( index )] = static_cast<float>( number.GetValue() );
                     ++count;
                 } );
            if ( !valid || count != 3 )
                return false;
            out = read;
            return true;
        }

        // Applies one entity's saved blocks. Every skip is a line in report.Problems naming `label`.
        void ApplyBlocks( entt::registry& registry, entt::entity entity, const Json::Node& blocks,
                          const std::string& label, const ReflectedTypeLookup& types, SaveGameLoadReport& report )
        {
            std::unordered_set<std::string> known;
            Serialize::ForEachReflectedComponentBlock(
                 [&]( const auto& row )
                 {
                     using Row       = std::remove_cvref_t<decltype( row )>;
                     using Component = typename Row::Component;
                     known.insert( row.Key );
                     const auto saved = blocks.Find( row.Key );
                     if ( !saved )
                         return;
                     if ( !registry.template has<Component>( entity ) )
                     {
                         report.Problems.push_back( std::format(
                              "{}: component '{}' is no longer on the entity; its saved fields were skipped",
                              label, row.Key ) );
                         return;
                     }
                     const Reflection::TypeInfo* type = types( row.TypeName );
                     if ( type == nullptr )
                     {
                         report.Problems.push_back(
                              std::format( "{}: component '{}': reflected type '{}' is not registered; skipped",
                                           label, row.Key, row.TypeName ) );
                         return;
                     }
                     Json::Object accepted;
                     if ( const auto fields = saved->Find( "Fields" ) )
                         fields->ForEachMember(
                              [&]( std::string_view name, const Json::Node& record )
                              {
                                  const auto        field = std::find_if( type->Fields.begin(), type->Fields.end(),
                                                                          [&]( const Reflection::FieldInfo& f )
                                                                          { return f.Name == name; } );
                                  const auto        savedType = record.Find( "Type" );
                                  const auto        value     = record.Find( "Value" );
                                  const std::string savedTypeName =
                                       savedType && savedType->AsString() ? savedType->AsString().GetValue() : "";
                                  if ( field == type->Fields.end() )
                                      report.Problems.push_back( std::format(
                                           "{}: {}.{} no longer exists; skipped", label, row.Key, name ) );
                                  else if ( !field->Meta.SaveGame )
                                      report.Problems.push_back(
                                           std::format( "{}: {}.{} is no longer marked SaveGame; skipped", label,
                                                        row.Key, name ) );
                                  else if ( savedTypeName != field->TypeName )
                                      report.Problems.push_back( std::format(
                                           "{}: {}.{} changed type from '{}' to '{}' since the save; skipped",
                                           label, row.Key, name, savedTypeName, field->TypeName ) );
                                  else if ( !value )
                                      report.Problems.push_back( std::format(
                                           "{}: {}.{} has no saved value; skipped", label, row.Key, name ) );
                                  else
                                      accepted[std::string( name )] = value->Raw();
                              } );
                     if ( accepted.size() == 0 )
                         return;
                     const std::size_t offered = accepted.size();
                     const Json::Value applied( std::move( accepted ) );
                     Json::Issues      issues;
                     Component&        component = registry.template get<Component>( entity );
                     Reflection::DeserializeReflected(
                          *type, BlockData( row, component ),
                          Json::Root( applied, Json::Path{}.Key( label ).Key( row.Key ) ), issues );
                     for ( const Json::Issue& issue : issues )
                         report.Problems.push_back( Json::Describe( issue ) + "; skipped" );
                     report.AppliedFields += offered > issues.size() ? offered - issues.size() : 0;
                 } );
            blocks.ForEachMember(
                 [&]( std::string_view key, const Json::Node& )
                 {
                     if ( !known.contains( std::string( key ) ) )
                         report.Problems.push_back( std::format(
                              "{}: component block '{}' is not a reflected block this build knows; skipped", label,
                              key ) );
                 } );
        }

        Common::BoolResultStr CheckEnvelope( const Json::Node& root, const std::string& what )
        {
            const auto format = root.Find( "Format" );
            if ( !format || !format->AsString() || format->AsString().GetValue() != SAVEGAME_FORMAT_NAME )
                return Common::MakeFormattedError<bool>( "{}: not a save game (Format is not '{}')", what,
                                                         SAVEGAME_FORMAT_NAME );
            const auto version = root.Find( "Version" );
            if ( !version || !version->AsInteger() )
                return Common::MakeFormattedError<bool>( "{}: save game has no integer Version", what );
            if ( version->AsInteger().GetValue() != SAVEGAME_FORMAT_VERSION )
                return Common::MakeFormattedError<bool>(
                     "{}: save game version {} refused, this build reads version {}", what,
                     version->AsInteger().GetValue(), SAVEGAME_FORMAT_VERSION );
            return Common::MakeSuccess( true );
        }

        bool IsValidSlotName( std::string_view slot )
        {
            if ( slot.empty() || slot == "." || slot == ".." || slot.back() == '.' || slot.back() == ' ' )
                return false;
            return std::none_of( slot.begin(), slot.end(),
                                 []( char c )
                                 {
                                     return static_cast<unsigned char>( c ) < 32 ||
                                            std::string_view( "<>:\"/\\|?*" ).find( c ) != std::string_view::npos;
                                 } );
        }

        std::filesystem::path UserDirectory( const std::filesystem::path& root, std::uint32_t userIndex )
        {
            return userIndex == 0 ? root : root / std::format( "User{}", userIndex );
        }
    } // namespace

    const Reflection::TypeInfo* RegistryTypeLookup( const std::string& typeName )
    {
        return Reflection::ReflectionRegistry::Get().Find( typeName );
    }

    Common::Json::Value CaptureSaveGame( const entt::registry& registry, entt::entity pawn,
                                         const SaveGameSceneIdentity& scene, const ReflectedTypeLookup& types )
    {
        Json::ObjectBuilder document;
        document.Set( "Format", SAVEGAME_FORMAT_NAME ).Set( "Version", SAVEGAME_FORMAT_VERSION );
        document.Set( "Scene", Json::ObjectBuilder{}.Set( "Guid", scene.Guid ).Set( "Name", scene.Name ).Build() );

        if ( pawn != entt::null && registry.valid( pawn ) && registry.has<ECS::TransformComponent>( pawn ) )
        {
            const auto& transform = registry.get<ECS::TransformComponent>( pawn );
            document.Set( "Pawn", Json::ObjectBuilder{}
                                       .Set( "Translation", transform.Translation )
                                       .Set( "Rotation", transform.Rotation )
                                       .Set( "Scale", transform.Scale )
                                       .Set( "Components", CaptureBlocks( registry, pawn, types ) )
                                       .Build() );
        }

        Json::Object entities;
        const auto   view = registry.view<const ECS::UUIDComponent>();
        for ( const entt::entity entity : view )
        {
            if ( entity == pawn )
                continue;
            Json::Object blocks = CaptureBlocks( registry, entity, types );
            if ( blocks.size() == 0 )
                continue;
            const std::string name =
                 registry.has<ECS::TagComponent>( entity ) ? registry.get<ECS::TagComponent>( entity ).Tag : "";
            entities[registry.get<ECS::UUIDComponent>( entity ).UUID.ToString()] =
                 Json::Value( Json::ObjectBuilder{}.Set( "Name", name ).Set( "Components", blocks ).Build() );
        }
        document.Set( "Entities", entities );
        return Json::Value( document.Build() );
    }

    Common::ResultStr<SaveGameSceneIdentity> SaveGameSceneOf( const Common::Json::Value& document )
    {
        const Json::Node root     = Json::Root( document );
        const auto       envelope = CheckEnvelope( root, "save game" );
        if ( !envelope )
            return Common::MakeFormattedError<SaveGameSceneIdentity>( "{}", envelope.GetError() );
        SaveGameSceneIdentity identity;
        if ( const auto scene = root.Find( "Scene" ) )
        {
            if ( const auto guid = scene->Find( "Guid" ); guid && guid->AsString() )
                identity.Guid = guid->AsString().GetValue();
            if ( const auto name = scene->Find( "Name" ); name && name->AsString() )
                identity.Name = name->AsString().GetValue();
        }
        return Common::MakeSuccess( identity );
    }

    Common::ResultStr<SaveGameLoadReport> ApplySaveGame( entt::registry& registry, entt::entity pawn,
                                                         const SaveGameSceneIdentity& scene,
                                                         const Common::Json::Value&   document,
                                                         const ReflectedTypeLookup&   types )
    {
        const auto saved = SaveGameSceneOf( document );
        if ( !saved )
            return Common::MakeFormattedError<SaveGameLoadReport>( "{}", saved.GetError() );
        if ( saved.GetValue().Guid != scene.Guid )
            return Common::MakeFormattedError<SaveGameLoadReport>(
                 "save game belongs to scene '{}' ({}), the loaded scene is '{}' ({}); open its level first",
                 saved.GetValue().Name, saved.GetValue().Guid, scene.Name, scene.Guid );

        SaveGameLoadReport report;
        const Json::Node   root = Json::Root( document );

        if ( const auto pawnRecord = root.Find( "Pawn" ) )
        {
            if ( pawn == entt::null || !registry.valid( pawn ) || !registry.has<ECS::TransformComponent>( pawn ) )
                report.Problems.push_back(
                     "the slot holds the player pawn but the scene has no player pawn; skipped" );
            else
            {
                auto&     transform   = registry.get<ECS::TransformComponent>( pawn );
                glm::vec3 translation = transform.Translation, rotation = transform.Rotation,
                          scale = transform.Scale;
                const auto t    = pawnRecord->Find( "Translation" );
                const auto r    = pawnRecord->Find( "Rotation" );
                const auto s    = pawnRecord->Find( "Scale" );
                if ( t && r && s && ReadVec3( *t, translation ) && ReadVec3( *r, rotation ) &&
                     ReadVec3( *s, scale ) )
                {
                    transform.Translation = translation;
                    transform.Rotation    = rotation;
                    transform.Scale       = scale;
                    report.PawnRestored   = true;
                }
                else
                    report.Problems.push_back( "player pawn: the saved transform is not three vec3s; skipped" );
                if ( const auto components = pawnRecord->Find( "Components" ) )
                    ApplyBlocks( registry, pawn, *components, "player pawn", types, report );
            }
        }

        std::unordered_map<std::string, entt::entity> byUuid;
        const auto                                    view = registry.view<const ECS::UUIDComponent>();
        for ( const entt::entity entity : view )
            byUuid.emplace( registry.get<ECS::UUIDComponent>( entity ).UUID.ToString(), entity );

        if ( const auto entities = root.Find( "Entities" ) )
            entities->ForEachMember(
                 [&]( std::string_view uuid, const Json::Node& record )
                 {
                     std::string name;
                     if ( const auto saved = record.Find( "Name" ); saved && saved->AsString() )
                         name = saved->AsString().GetValue();
                     const std::string label = std::format( "entity '{}' ({})", name, uuid );
                     const auto        found = byUuid.find( std::string( uuid ) );
                     if ( found == byUuid.end() )
                     {
                         report.Problems.push_back( label +
                                                    " is not in the scene; its saved fields were skipped" );
                         return;
                     }
                     if ( const auto components = record.Find( "Components" ) )
                         ApplyBlocks( registry, found->second, *components, label, types, report );
                 } );
        return Common::MakeSuccess( std::move( report ) );
    }

    Common::ResultStr<std::filesystem::path> SaveGameSlotPath( const std::filesystem::path& root,
                                                               std::string_view slot, std::uint32_t userIndex )
    {
        if ( root.empty() )
            return Common::MakeFormattedError<std::filesystem::path>( "save game slot '{}': no save root", slot );
        if ( !IsValidSlotName( slot ) )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "save game slot '{}' is not a valid file name (empty, '.', '..', a trailing dot or space, a path "
                 "separator or one of <>:\"|?*)",
                 slot );
        return Common::MakeSuccess( UserDirectory( root, userIndex ) /
                                    ( std::string( slot ) + std::string( SAVEGAME_EXTENSION ) ) );
    }

    Common::BoolResultStr WriteSaveGameSlot( const std::filesystem::path& root, std::string_view slot,
                                             std::uint32_t userIndex, const Common::Json::Value& document )
    {
        const auto path = SaveGameSlotPath( root, slot, userIndex );
        if ( !path )
            return Common::MakeFormattedError<bool>( "{}", path.GetError() );
        std::error_code ec;
        std::filesystem::create_directories( path.GetValue().parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "save game slot '{}': could not create {}: {}", slot,
                                                     path.GetValue().parent_path().string(), ec.message() );
        const auto text = Json::WriteCanonical( document );
        if ( !text )
            return Common::MakeFormattedError<bool>( "save game slot '{}': {}", slot, text.GetError() );
        const auto written =
             Common::Utils::FileSystem::WriteContentToFileAtomic( path.GetValue(), text.GetValue() );
        if ( !written )
            return Common::MakeFormattedError<bool>(
                 "save game slot '{}' not written, the previous save is kept: {}", slot, written.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<Common::Json::Value> ReadSaveGameSlot( const std::filesystem::path& root,
                                                             std::string_view slot, std::uint32_t userIndex )
    {
        const auto path = SaveGameSlotPath( root, slot, userIndex );
        if ( !path )
            return Common::MakeFormattedError<Json::Value>( "{}", path.GetError() );
        const std::string what = path.GetValue().string();
        std::error_code   ec;
        if ( !std::filesystem::is_regular_file( path.GetValue(), ec ) )
            return Common::MakeFormattedError<Json::Value>( "no save game in slot '{}' ({})", slot, what );
        const auto text = Common::Utils::FileSystem::ReadFileContent( path.GetValue() );
        if ( !text )
            return Common::MakeFormattedError<Json::Value>( "{}: could not be read: {}", what, text.GetError() );
        auto parsed = Json::Parse( text.GetValue() );
        if ( !parsed )
            return Common::MakeFormattedError<Json::Value>( "{}: {}", what, parsed.GetError() );
        Json::Value document = parsed.ExtractValue();
        const auto  envelope = CheckEnvelope( Json::Root( document ), what );
        if ( !envelope )
            return Common::MakeFormattedError<Json::Value>( "{}", envelope.GetError() );
        return Common::MakeSuccess( std::move( document ) );
    }

    bool DoesSaveGameExist( const std::filesystem::path& root, std::string_view slot, std::uint32_t userIndex )
    {
        const auto      path = SaveGameSlotPath( root, slot, userIndex );
        std::error_code ec;
        return path && std::filesystem::is_regular_file( path.GetValue(), ec );
    }

    Common::BoolResultStr DeleteGameInSlot( const std::filesystem::path& root, std::string_view slot,
                                            std::uint32_t userIndex )
    {
        const auto path = SaveGameSlotPath( root, slot, userIndex );
        if ( !path )
            return Common::MakeFormattedError<bool>( "{}", path.GetError() );
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( path.GetValue(), ec ) )
            return Common::MakeFormattedError<bool>( "no save game in slot '{}' ({})", slot,
                                                     path.GetValue().string() );
        if ( !std::filesystem::remove( path.GetValue(), ec ) || ec )
            return Common::MakeFormattedError<bool>( "save game slot '{}' ({}) could not be deleted: {}", slot,
                                                     path.GetValue().string(), ec.message() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<std::vector<std::string>> ListSaveGameSlots( const std::filesystem::path& root,
                                                                   std::uint32_t                userIndex )
    {
        std::vector<std::string>    slots;
        const std::filesystem::path directory = UserDirectory( root, userIndex );
        std::error_code             ec;
        if ( !std::filesystem::is_directory( directory, ec ) )
            return Common::MakeSuccess( std::move( slots ) );
        for ( std::filesystem::directory_iterator it( directory, ec ), end; !ec && it != end; it.increment( ec ) )
            if ( it->is_regular_file() && it->path().extension() == SAVEGAME_EXTENSION )
                slots.push_back( it->path().stem().string() );
        if ( ec )
            return Common::MakeFormattedError<std::vector<std::string>>(
                 "save games in {} could not be listed: {}", directory.string(), ec.message() );
        std::sort( slots.begin(), slots.end() );
        return Common::MakeSuccess( std::move( slots ) );
    }

    void SetSaveGameRoot( const std::filesystem::path& root )
    {
        const std::lock_guard lock( RootMutex() );
        DeclaredRoot() = root;
    }

    Common::ResultStr<std::filesystem::path> SaveGameRoot()
    {
        {
            const std::lock_guard lock( RootMutex() );
            if ( !DeclaredRoot().empty() )
                return Common::MakeSuccess( DeclaredRoot() );
        }
        if ( !Project::ProjectContext::HasProject() )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "save games: no project is open and the host declared no save root" );
        return Common::MakeSuccess( std::filesystem::path( Project::ProjectContext::Directory() ) / "Saved" /
                                    "SaveGames" );
    }

    SaveGameSceneIdentity SceneIdentityOf( const Scene& scene )
    {
        const auto& header = scene.GetAssetHeader();
        return SaveGameSceneIdentity{ header ? header->Guid : std::string{}, scene.GetSceneName() };
    }

    Common::BoolResultStr SaveGameToSlot( const Scene& scene, std::string_view slot, std::uint32_t userIndex )
    {
        const auto root = SaveGameRoot();
        if ( !root )
            return Common::MakeFormattedError<bool>( "{}", root.GetError() );
        return WriteSaveGameSlot( root.GetValue(), slot, userIndex,
                                  CaptureSaveGame( scene.GetRegistry(), scene.GetPlayerPawn(),
                                                   SceneIdentityOf( scene ), RegistryTypeLookup ) );
    }

    Common::ResultStr<SaveGameLoadReport> LoadGameFromSlot( Scene& scene, std::string_view slot,
                                                            std::uint32_t userIndex )
    {
        const auto root = SaveGameRoot();
        if ( !root )
            return Common::MakeFormattedError<SaveGameLoadReport>( "{}", root.GetError() );
        const auto document = ReadSaveGameSlot( root.GetValue(), slot, userIndex );
        if ( !document )
            return Common::MakeFormattedError<SaveGameLoadReport>( "{}", document.GetError() );
        return ApplySaveGame( scene.GetRegistry(), scene.GetPlayerPawn(), SceneIdentityOf( scene ),
                              document.GetValue(), RegistryTypeLookup );
    }

    Common::ResultStr<bool> DoesSaveGameExist( std::string_view slot, std::uint32_t userIndex )
    {
        const auto root = SaveGameRoot();
        if ( !root )
            return Common::MakeFormattedError<bool>( "{}", root.GetError() );
        return Common::MakeSuccess( DoesSaveGameExist( root.GetValue(), slot, userIndex ) );
    }

    Common::BoolResultStr DeleteGameInSlot( std::string_view slot, std::uint32_t userIndex )
    {
        const auto root = SaveGameRoot();
        if ( !root )
            return Common::MakeFormattedError<bool>( "{}", root.GetError() );
        return DeleteGameInSlot( root.GetValue(), slot, userIndex );
    }

    Common::ResultStr<std::vector<std::string>> ListSaveGameSlots( std::uint32_t userIndex )
    {
        const auto root = SaveGameRoot();
        if ( !root )
            return Common::MakeFormattedError<std::vector<std::string>>( "{}", root.GetError() );
        return ListSaveGameSlots( root.GetValue(), userIndex );
    }
} // namespace Desert::Core
