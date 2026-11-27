#include <Engine/Core/SceneUpgrade/Steps/V74FilmicDefault.hpp>

#include <Engine/Core/SceneUpgrade/Steps/StepEdits.hpp>

#include <format>
#include <string>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        using UpgradeEdits::DropKey;
        using UpgradeEdits::EditBlock;

        // The step numbers, frozen: a step restates the numbers of ITS generation, so a later renumbering of an
        // enum cannot change what an old file becomes.
        constexpr int kTonemapACES                 = 0; // Core::TonemapOperator::ACES at v74
        constexpr int kTonemapFilmic               = 2; // Core::TonemapOperator::Filmic at v74
    } // namespace

    FilmicDefaultReport MigrateFilmicDefaultV73ToV74( std::vector<Assets::EntityData>& entities )
    {
        constexpr int       kACES   = kTonemapACES;
        constexpr int       kFilmic = kTonemapFilmic;
        FilmicDefaultReport report;

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
                           bool changed = false;
                           if ( const auto op = settings.get( "Tonemapper" ); op.has_value() )
                           {
                               const auto number = op.value().to_int();
                               if ( number.has_value() && number.value() == kACES )
                               {
                                   DropKey( settings, "Tonemapper" );
                                   settings["Tonemapper"] = rfl::Generic( kFilmic );
                                   ++report.Restated;
                                   changed = true;
                               }
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

    StepOutcome RunFilmicDefault( UpgradeContext& context )
    {
        FilmicDefaultReport report = MigrateFilmicDefaultV73ToV74( context.Entities );
        StepOutcome         outcome;
        outcome.Changed = report.Restated;
        outcome.Summary = std::format( "{} Tonemapper ACES re-stated as Filmic", report.Restated );
        outcome.Refused = std::move( report.Refused );
        return outcome;
    }
} // namespace Desert::Core
