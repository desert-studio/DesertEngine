#include "SettingsCanonical.hpp"

#include <Engine/Core/SceneSettings.hpp>
// The engine's own reflection table and serializer, linked into this tool (premake5.lua) so that
// "canonical" means "the bytes the saver writes" rather than a field list maintained twice.
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Json/Document.hpp>

#include <rflcpp/rfl/json.hpp>

#include <optional>
#include <string>
#include <utility>

namespace Desert::Migration
{
    SettingsCanonicalisationReport CanonicaliseSettings( std::optional<rfl::Generic>& settings )
    {
        SettingsCanonicalisationReport report;

        const auto* type = Reflection::ReflectionRegistry::Get().Find( "SceneSettings" );
        if ( type == nullptr )
        {
            // The table is linked into this tool on purpose (see premake5.lua). If it is empty the tool
            // cannot know what canonical means, and guessing would write a Settings block out of nothing.
            LOG_ERROR( "[SceneMigration] the SceneSettings reflection table is not registered in this "
                       "build of SceneMigrator - the Settings block is left exactly as it is, and the "
                       "scene will not be byte-stable through a save" );
            report.Refused = true;
            return report;
        }

        rfl::Generic::Object stated;
        if ( !settings.has_value() )
        {
            report.BlockCreated = true;
        }
        else if ( const auto fields = settings.value().to_object(); fields.has_value() )
        {
            stated = fields.value();
        }
        else
        {
            LOG_WARN( "[SceneMigration] the Settings block is not an object - it cannot be canonicalised "
                      "and stays as it is" );
            report.Refused = true;
            return report;
        }

        // THROUGH THE ENGINE'S OWN PAIR OF FUNCTIONS, in both directions. Reading into a default-
        // constructed SceneSettings is exactly what the loader does (a key the file omits keeps the C++
        // default), and writing it back out is exactly what the saver does - so the result is the saver's
        // bytes by construction rather than by a list maintained here.
        // A wrong-typed value would be replaced by its default in the rewritten block, which is not a
        // canonicalisation but a data loss: the block is refused and named instead.
        Core::SceneSettings       values;
        const Common::Json::Value statedValue( stated );
        Common::Json::Issues      issues;
        Reflection::DeserializeReflected(
             *type, &values, Common::Json::Root( statedValue, Common::Json::Path().Key( "Settings" ) ), issues );
        if ( !issues.empty() )
        {
            Common::Json::ReportIssues( issues, "[SceneMigration] the Settings block stays as it is" );
            report.Refused = true;
            return report;
        }
        rfl::Generic::Object canonical = Reflection::SerializeReflected( *type, &values );

        // AN ASSET HANDLE HAS TWO ON-DISK FORMS AND THIS TOOL CAN ONLY PRODUCE ONE OF THEM. The saver
        // passes an asset RESOLVER, which writes a handle as a PATH STRING; SerializeReflected without one
        // writes the raw uint64. So the raw number above is never what the file should end up carrying.
        //
        //   * the file already states the field -> keep its text verbatim, whatever form it is in;
        //   * the file does not state it and the value is UNSET -> the empty path, which is exactly what
        //     the resolver produces for handle 0 and what every scene that states this field carries;
        //   * the file does not state it and the value is SET -> this tool cannot name the asset, so it
        //     refuses the whole block rather than writing a number a loader would read as a handle.
        //
        // The middle case is not a nicety: 68 of the 82 scenes did not state SplashSprite, so without it
        // canonicalising them ADDED a numeric asset reference to every one — caught by
        // Desert/Tests/Engine/AssetReferenceCensus, which is exactly the sweep doing its job.
        for ( const auto& field : type->Fields )
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

            if ( unset )
            {
                canonical[field.Name] = std::string();
                continue;
            }

            LOG_ERROR( "[SceneMigration] the Settings field '{0}' holds a set asset handle that this scene "
                       "does not state, and this tool has no asset manager to name it with - the Settings "
                       "block is left exactly as it is",
                       field.Name );
            report.Refused = true;
            return report;
        }

        for ( const auto& [key, value] : canonical )
        {
            const auto before = stated.get( key );
            if ( !before.has_value() )
                ++report.KeysAdded;
            else if ( rfl::json::write( before.value() ) != rfl::json::write( value ) )
                ++report.ValuesRestated;
        }

        settings = rfl::Generic( std::move( canonical ) );
        return report;
    }
} // namespace Desert::Migration
