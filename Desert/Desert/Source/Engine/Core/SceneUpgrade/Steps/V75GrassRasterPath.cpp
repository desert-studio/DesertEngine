#include <Engine/Core/SceneUpgrade/Steps/V75GrassRasterPath.hpp>

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
        constexpr int kGrassRasterScalability      = 0; // Core::GrassRasterPath::Scalability at v75
        constexpr int kGrassRasterVisibilityBuffer = 2; // Core::GrassRasterPath::VisibilityBuffer at v75
    } // namespace

    GrassRasterPathReport MigrateGrassRasterPathV74ToV75( std::vector<Assets::EntityData>& entities )
    {
        constexpr int         kScalability      = kGrassRasterScalability;
        constexpr int         kVisibilityBuffer = kGrassRasterVisibilityBuffer;
        GrassRasterPathReport report;

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
                           const auto           former   = settings.get( "GrassVisibilityBuffer" );
                           if ( !former.has_value() )
                               return false;
                           const auto flag = former.value().to_bool();
                           if ( !flag.has_value() )
                           {
                               report.Refused.push_back( std::format(
                                    "{}: PostProcessVolume.Settings.GrassVisibilityBuffer {} is not a bool", who,
                                    rfl::json::write( former.value() ) ) );
                               return false;
                           }
                           int path = kScalability;
                           if ( flag.value() )
                               path = kVisibilityBuffer;
                           DropKey( settings, "GrassVisibilityBuffer" );
                           settings["GrassRaster"] = rfl::Generic( path );
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

    StepOutcome RunGrassRasterPath( UpgradeContext& context )
    {
        GrassRasterPathReport report = MigrateGrassRasterPathV74ToV75( context.Entities );
        StepOutcome           outcome;
        outcome.Changed = report.Restated;
        outcome.Summary = std::format( "{} GrassVisibilityBuffer bool(s) re-stated as GrassRaster: false is "
                                       "Scalability, true VisibilityBuffer",
                                       report.Restated );
        outcome.Refused = std::move( report.Refused );
        return outcome;
    }
} // namespace Desert::Core
