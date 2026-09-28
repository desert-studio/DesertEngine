#include "SettingsCanonical.hpp"
#include "SceneMigration.hpp"

#include <Engine/Core/SceneSettings.hpp>
// The list of blocks that are their reflection and nothing else - the same rows ComponentRegistry
// registers its serializers from, so this pass cannot cover a block the saver writes by hand.
#include <Engine/Core/Serialize/ReflectedComponentBlocks.hpp>
// The engine's own reflection table and serializer, linked into this tool (premake5.lua) so that
// "canonical" means "the bytes the saver writes" rather than a field list maintained twice.
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Json/Document.hpp>

#include <rflcpp/rfl/json.hpp>

#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace Desert::Migration
{
    namespace
    {
        BlockCanonicalisationReport Refuse( BlockCanonicalisationReport report, std::string why )
        {
            report.Refused    = true;
            report.RefusedWhy = std::move( why );
            return report;
        }

        // The body, for a block already read into `values` (a default-constructed instance of `type`) - which
        // is the one step that needs the C++ type, and why the callers are per-type.
        BlockCanonicalisationReport CanonicaliseInto( const Reflection::TypeInfo& type, void* values,
                                                      std::optional<rfl::Generic>& block,
                                                      const std::string&           where )
        {
            BlockCanonicalisationReport report;

            rfl::Generic::Object stated;
            if ( !block.has_value() )
                report.BlockCreated = true;
            else if ( const auto fields = block.value().to_object(); fields.has_value() )
                stated = fields.value();
            else
                return Refuse( report, std::format( "{} is not an object", where ) );

            // THROUGH THE ENGINE'S OWN PAIR OF FUNCTIONS, in both directions: reading into a default-constructed
            // value is what the loader does (a key the file omits keeps the C++ default), writing it back is what
            // the saver does - so the result is the saver's bytes by construction rather than by a list kept here.
            // A wrong-typed value would come back as its default, which is data loss, not canonicalisation.
            const Common::Json::Value statedValue( stated );
            Common::Json::Issues      issues;
            Reflection::DeserializeReflected(
                 type, values, Common::Json::Root( statedValue, Common::Json::Path().Key( where ) ), issues );
            if ( !issues.empty() )
            {
                Common::Json::ReportIssues( issues, "[SceneMigration] the block stays as it is" );
                return Refuse( report,
                               std::format( "{} states a value of the wrong type (logged above)", where ) );
            }
            rfl::Generic::Object canonical = Reflection::SerializeReflected( type, values );

            for ( const auto& [key, value] : stated )
                if ( !canonical.get( key ).has_value() )
                    return Refuse( report, std::format( "{} states '{}', which this build does not declare - the "
                                                        "saver keeps such a key where it stood and this pass "
                                                        "does not restate that merge",
                                                        where, key ) );

            // The saver's spelling of an UNSET handle, from the saver's own writer: a resolver that names no
            // asset is what MakeAssetResolver answers for handle 0 ("" for GUID and path), and WriteField lays
            // that out in whichever form the field's asset type takes ({Guid, Path} for a texture).
            Reflection::AssetResolver namesNothing;
            namesNothing.ToPath                  = []( uint64_t, const std::string& ) { return std::string(); };
            namesNothing.ToGuid                  = []( uint64_t, const std::string& ) { return std::string(); };
            const rfl::Generic::Object unsetForm = Reflection::SerializeReflected( type, values, &namesNothing );

            // AN ASSET HANDLE HAS TWO ON-DISK FORMS AND THIS TOOL CAN ONLY PRODUCE ONE OF THEM (the raw number,
            // which is never what the file should carry):
            //   * stated -> its text verbatim, whatever form it is in;
            //   * unstated and UNSET -> the saver's form for handle 0 (without it canonicalising the 68 scenes
            //     that did not state SplashSprite ADDED a numeric asset reference to each - AssetReferenceCensus);
            //   * unstated and SET -> refused: this tool cannot name the asset.
            for ( const auto& field : type.Fields )
            {
                if ( field.Type != Reflection::FieldType::AssetHandle )
                    continue;

                if ( const auto asStated = stated.get( field.Name ); asStated.has_value() )
                {
                    canonical[field.Name] = asStated.value();
                    continue;
                }

                bool unset = false;
                if ( const auto asWritten = canonical.get( field.Name ); asWritten.has_value() )
                    if ( const auto asNumber = asWritten.value().to_int64(); asNumber.has_value() )
                        unset = asNumber.value() == 0;
                if ( !unset )
                    return Refuse( report, std::format( "{}.{} holds a set asset handle the file does not state, "
                                                        "and this tool has no asset manager to name it with",
                                                        where, field.Name ) );
                canonical[field.Name] = unsetForm.get( field.Name ).value();
            }

            for ( const auto& [key, value] : canonical )
            {
                const auto before = stated.get( key );
                if ( !before.has_value() )
                    ++report.KeysAdded;
                else if ( rfl::json::write( before.value() ) != rfl::json::write( value ) )
                    ++report.ValuesRestated;
            }

            // Key ORDER is text too: a block stating its keys in another order than the schema is rewritten
            // even when no key is added and no value restated.
            report.Rewritten = report.BlockCreated || rfl::json::write( rfl::Generic( stated ) ) !=
                                                           rfl::json::write( rfl::Generic( canonical ) );
            block = rfl::Generic( std::move( canonical ) );
            return report;
        }

        template <class TData>
        BlockCanonicalisationReport CanonicaliseAs( const Reflection::TypeInfo&  type,
                                                    std::optional<rfl::Generic>& block, const std::string& where )
        {
            TData values{};
            return CanonicaliseInto( type, &values, block, where );
        }

        using BlockCanonicaliser = BlockCanonicalisationReport ( * )( const Reflection::TypeInfo&,
                                                                      std::optional<rfl::Generic>&,
                                                                      const std::string& );
        struct ReflectedBlockRow
        {
            const char*        TypeName;
            BlockCanonicaliser Canonicalise;
        };

        // Block key -> its reflected type, from ReflectedComponentBlocks.hpp (the registry's own rows).
        const std::map<std::string, ReflectedBlockRow>& ReflectedBlocksByKey()
        {
            static const std::map<std::string, ReflectedBlockRow> rows = []
            {
                std::map<std::string, ReflectedBlockRow> out;
                Core::Serialize::ForEachReflectedComponentBlock(
                     [&]( const auto& row )
                     {
                         using Data = typename std::decay_t<decltype( row )>::Data;
                         out.emplace( row.Key, ReflectedBlockRow{ row.TypeName, &CanonicaliseAs<Data> } );
                     } );
                return out;
            }();
            return rows;
        }

        BlockCanonicalisationReport CanonicaliseRow( const char* typeName, BlockCanonicaliser canonicalise,
                                                     std::optional<rfl::Generic>& block, const std::string& where )
        {
            const auto* type = Reflection::ReflectionRegistry::Get().Find( typeName );
            if ( type == nullptr )
            {
                // The table is linked into this tool on purpose (see premake5.lua). If it is empty the tool
                // cannot know what canonical means, and guessing would write a block out of nothing.
                BlockCanonicalisationReport report;
                return Refuse( report, std::format( "{}: the reflected type '{}' is not registered in this build",
                                                    where, typeName ) );
            }
            return canonicalise( *type, block, where );
        }
    } // namespace

    BlockCanonicalisationReport CanonicaliseReflectedBlock( const char*                  typeName,
                                                            std::optional<rfl::Generic>& block,
                                                            const std::string&           where )
    {
        for ( const auto& [key, row] : ReflectedBlocksByKey() )
            if ( std::string_view( row.TypeName ) == typeName )
                return CanonicaliseRow( row.TypeName, row.Canonicalise, block, where );
        if ( std::string_view( typeName ) == "SceneSettings" )
            return CanonicaliseRow( typeName, &CanonicaliseAs<Core::SceneSettings>, block, where );
        BlockCanonicalisationReport report;
        return Refuse( report, std::format( "{}: '{}' is not a reflected block type", where, typeName ) );
    }

    BlockCanonicalisationReport CanonicaliseSettings( std::optional<rfl::Generic>& settings )
    {
        auto report = CanonicaliseReflectedBlock( "SceneSettings", settings, "Settings" );
        if ( report.Refused )
            LOG_ERROR( "[SceneMigration] {} - the Settings block is left exactly as it is", report.RefusedWhy );
        return report;
    }

    SceneCanonicalisationReport CanonicaliseScene( SceneSerialized& scene )
    {
        SceneCanonicalisationReport report;
        // All-or-nothing: work on a copy, keep it only if nothing was refused.
        SceneSerialized canonical = scene;

        const auto account = [&report]( const BlockCanonicalisationReport& block )
        {
            report.KeysAdded += block.KeysAdded;
            report.ValuesRestated += block.ValuesRestated;
            if ( block.Rewritten )
                ++report.BlocksRestated;
        };

        const auto settings = CanonicaliseReflectedBlock( "SceneSettings", canonical.Settings, "Settings" );
        if ( settings.Refused )
        {
            report.Refused = settings.RefusedWhy;
            return report;
        }
        account( settings );

        const auto& rows = ReflectedBlocksByKey();
        for ( auto& entity : canonical.Entities )
        {
            const uint64_t id = static_cast<uint64_t>( entity.id.value_or( Common::UUID( 0 ) ) );
            for ( auto& [key, value] : entity.Components )
            {
                const auto row = rows.find( key );
                if ( row == rows.end() )
                    continue; // a hand-written block: not this pass's (see the header's NOT COVERED)
                std::optional<rfl::Generic> block = value;
                const auto done = CanonicaliseRow( row->second.TypeName, row->second.Canonicalise, block,
                                                   std::format( "entity {} / {}", id, key ) );
                if ( done.Refused )
                {
                    report.Refused = done.RefusedWhy;
                    return report;
                }
                account( done );
                value = std::move( block ).value();
            }
        }

        scene = std::move( canonical );
        return report;
    }
} // namespace Desert::Migration
