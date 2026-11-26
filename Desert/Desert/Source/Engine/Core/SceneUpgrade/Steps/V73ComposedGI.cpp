#include <Engine/Core/SceneUpgrade/Steps/V73ComposedGI.hpp>

#include <Engine/Core/SceneUpgrade/Steps/StepEdits.hpp>
#include <Common/Settings/RenderMethods.hpp>

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

    ComposedGIReport MigrateComposedGIV72ToV73( std::vector<Assets::EntityData>& entities )
    {
        using Common::Scalability::GIRequest;
        constexpr int    kScreenSpace = static_cast<int>( GIRequest::ScreenSpace );
        constexpr int    kOff         = static_cast<int>( GIRequest::Off );
        ComposedGIReport report;

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
                           bool                 changed  = false;
                           if ( const auto gi = settings.get( "GlobalIlluminationMethod" ); gi.has_value() )
                           {
                               const auto number = gi.value().to_int();
                               if ( number.has_value() && number.value() == kScreenSpace )
                               {
                                   DropKey( settings, "GlobalIlluminationMethod" );
                                   settings["GlobalIlluminationMethod"] = rfl::Generic( kOff );
                                   ++report.Restated;
                                   changed = true;
                               }
                           }
                           if ( settings.get( "GIIntensity" ).has_value() )
                           {
                               DropKey( settings, "GIIntensity" );
                               ++report.Dropped;
                               changed = true;
                           }
                           if ( !changed )
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

    StepOutcome RunComposedGI( UpgradeContext& context )
    {
        // The same PostProcessVolume keys, after the v72 re-statement they follow.
        ComposedGIReport report = MigrateComposedGIV72ToV73( context.Entities );
        StepOutcome      outcome;
        outcome.Changed = report.Restated + report.Dropped;
        outcome.Summary = std::format( "{} GI ScreenSpace request(s) re-stated as Off, {} GIIntensity dropped: GI "
                                       "comes from the method alone",
                                       report.Restated, report.Dropped );
        outcome.Refused = std::move( report.Refused );
        return outcome;
    }
} // namespace Desert::Core
