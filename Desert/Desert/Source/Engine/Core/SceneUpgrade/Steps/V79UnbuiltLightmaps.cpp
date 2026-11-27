#include <Engine/Core/SceneUpgrade/Steps/V79UnbuiltLightmaps.hpp>

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

    UnbuiltLightmapsReport MigrateUnbuiltLightmapsV78ToV79( std::vector<Assets::EntityData>& entities )
    {
        using Common::Scalability::GIRequest;
        constexpr int          kLightmaps = static_cast<int>( GIRequest::Lightmaps );
        constexpr int          kOff       = static_cast<int>( GIRequest::Off );
        UnbuiltLightmapsReport report;

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
                           const auto           gi       = settings.get( "GlobalIlluminationMethod" );
                           if ( !gi.has_value() )
                               return false;
                           const auto number = gi.value().to_int();
                           if ( !number.has_value() || number.value() != kLightmaps )
                               return false;
                           DropKey( settings, "GlobalIlluminationMethod" );
                           settings["GlobalIlluminationMethod"] = rfl::Generic( kOff );
                           ++report.Restated;
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

    StepOutcome RunUnbuiltLightmaps( UpgradeContext& context )
    {
        UnbuiltLightmapsReport report = MigrateUnbuiltLightmapsV78ToV79( context.Entities );
        StepOutcome            outcome;
        outcome.Changed = report.Restated;
        outcome.Summary =
             std::format( "{} GI Lightmaps request(s) re-stated as Off: Lightmaps was never built and "
                          "rendered Off",
                          report.Restated );
        outcome.Refused = std::move( report.Refused );
        return outcome;
    }
} // namespace Desert::Core
