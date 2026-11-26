#include <Engine/Core/SceneUpgrade/Steps/V72TracedRequest.hpp>

#include <Engine/Core/SceneUpgrade/Steps/StepEdits.hpp>
#include <Common/Settings/RenderMethods.hpp>

#include <array>
#include <span>

#include <format>
#include <string>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        using UpgradeEdits::DropKey;
        using UpgradeEdits::EditBlock;
    } // namespace

    TracedRequestReport MigrateTracedRequestV71ToV72( std::vector<Assets::EntityData>& entities )
    {
        using Common::Scalability::GIRequest;
        using Common::Scalability::ReflectionRequest;
        TracedRequestReport report;

        // The v64..v71 ladders by their numbers, as today's requests: both tracers are Traced.
        constexpr std::array<int, 7> kGIFromOld{
             static_cast<int>( GIRequest::Off ),         static_cast<int>( GIRequest::Lightmaps ),
             static_cast<int>( GIRequest::ScreenSpace ), static_cast<int>( GIRequest::ProbeVolume ),
             static_cast<int>( GIRequest::Traced ),      static_cast<int>( GIRequest::Traced ),
             static_cast<int>( GIRequest::PathTraced ) };
        constexpr std::array<int, 6> kReflectionsFromOld{
             static_cast<int>( ReflectionRequest::Off ),         static_cast<int>( ReflectionRequest::Captures ),
             static_cast<int>( ReflectionRequest::ScreenSpace ), static_cast<int>( ReflectionRequest::Traced ),
             static_cast<int>( ReflectionRequest::Traced ),
             static_cast<int>( ReflectionRequest::PathTraced ) };

        // One key of one Settings object; false when it is absent or already on the new number.
        const auto restateKey = [&]( rfl::Generic::Object& settings, const char* key, std::span<const int> table,
                                     const std::string& who, bool& refused )
        {
            const auto stated = settings.get( key );
            if ( !stated.has_value() )
                return false;
            const auto old = stated.value().to_int();
            if ( !old.has_value() || old.value() < 0 || old.value() >= static_cast<int>( table.size() ) )
            {
                report.Refused.push_back( std::format(
                     "{}: PostProcessVolume.Settings.{} {} is not on the v71 ladder (0 .. {} PathTraced)", who,
                     key, rfl::json::write( stated.value() ), table.size() - 1 ) );
                refused = true;
                return false;
            }
            const int now = table[static_cast<std::size_t>( old.value() )];
            if ( now == old.value() )
                return false;
            DropKey( settings, key );
            settings[key] = rfl::Generic( now );
            ++report.Restated;
            return true;
        };

        const auto restate = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock( components, "PostProcessVolume",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto stated = block.get( "Settings" );
                           if ( !stated.has_value() )
                               return false;
                           auto fields = stated.value().to_object();
                           if ( !fields.has_value() )
                           {
                               report.Refused.push_back(
                                    std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                                 rfl::json::write( stated.value() ) ) );
                               return false;
                           }
                           rfl::Generic::Object settings = std::move( fields.value() );
                           bool                 refused  = false;
                           const bool           gi       = restateKey( settings, "GlobalIlluminationMethod",
                                                                       std::span<const int>( kGIFromOld ), who,
                                                                       refused );
                           const bool reflections =
                                restateKey( settings, "ReflectionsMethod",
                                            std::span<const int>( kReflectionsFromOld ), who, refused );
                           if ( refused || ( !gi && !reflections ) )
                               return false;
                           DropKey( block, "Settings" );
                           block["Settings"] = rfl::Generic( std::move( settings ) );
                           return true;
                       } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            restate( entity.Components, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    restate( override_.Components, who );
        }
        return report;
    }

    StepOutcome RunTracedRequest( UpgradeContext& context )
    {
        TracedRequestReport report = MigrateTracedRequestV71ToV72( context.Entities );
        StepOutcome         outcome;
        outcome.Changed = report.Restated;
        outcome.Summary = std::format(
             "{} GI / reflection method(s) re-stated as requests: SoftwareTraced / HardwareTraced are Traced",
             report.Restated );
        outcome.Refused = std::move( report.Refused );
        return outcome;
    }
} // namespace Desert::Core
