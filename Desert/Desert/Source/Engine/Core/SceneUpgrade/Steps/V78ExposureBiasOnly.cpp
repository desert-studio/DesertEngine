#include <Engine/Core/SceneUpgrade/Steps/V78ExposureBiasOnly.hpp>

#include <Engine/Core/SceneUpgrade/Steps/StepEdits.hpp>

#include <cmath>

#include <format>
#include <string>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        using UpgradeEdits::DropKey;
        using UpgradeEdits::EditBlock;
        using UpgradeEdits::NumberOf;

        // The step numbers, frozen: a step restates the numbers of ITS generation, so a later renumbering of an
        // enum cannot change what an old file becomes.
        constexpr double kExposureMiddleGreyV78       = 0.18; // Graphic::kExposureMiddleGrey at v78
    } // namespace

    ExposureKeyReport MigrateExposureKeyV77ToV78( std::vector<Assets::EntityData>& entities )
    {
        ExposureKeyReport report;

        // @p delta: a prefab override, whose absent bias is the prefab's and not 0.
        const auto fold = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who, bool delta )
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
                           const auto           former   = settings.get( "AutoExposureKey" );
                           if ( !former.has_value() )
                               return false;
                           const std::optional<double> key = NumberOf( former.value() );
                           if ( !key.has_value() || !( key.value() > 0.0 ) )
                           {
                               report.Refused.push_back( std::format(
                                    "{}: PostProcessVolume.Settings.AutoExposureKey {} is not a positive number",
                                    who, rfl::json::write( former.value() ) ) );
                               return false;
                           }
                           // Rounded to 1e-6 EV: a float key folds to what it stated (0.18000000715 to no offset,
                           // 0.04500000178 to -2).
                           const double folded   = std::log2( key.value() / kExposureMiddleGreyV78 );
                           const double offset   = std::round( folded * 1.0e6 ) / 1.0e6;
                           const auto   biasKept = settings.get( "AutoExposureBias" );
                           double       bias     = 0.0;
                           if ( biasKept.has_value() )
                           {
                               const std::optional<double> statedBias = NumberOf( biasKept.value() );
                               if ( !statedBias.has_value() )
                               {
                                   report.Refused.push_back( std::format(
                                        "{}: PostProcessVolume.Settings.AutoExposureBias {} is not a number", who,
                                        rfl::json::write( biasKept.value() ) ) );
                                   return false;
                               }
                               bias = statedBias.value();
                           }
                           else if ( delta && offset != 0.0 )
                           {
                               report.Refused.push_back( std::format(
                                    "{}: a prefab override states PostProcessVolume.Settings.AutoExposureKey {} "
                                    "without AutoExposureBias - state the bias it inherits, then migrate",
                                    who, key.value() ) );
                               return false;
                           }
                           DropKey( settings, "AutoExposureKey" );
                           if ( offset != 0.0 )
                           {
                               DropKey( settings, "AutoExposureBias" );
                               settings["AutoExposureBias"] = rfl::Generic( bias + offset );
                           }
                           ++report.Folded;
                           DropKey( block, "Settings" );
                           block["Settings"] = rfl::Generic( std::move( settings ) );
                           return true;
                       } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            fold( entity.Components, who, false );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    fold( override_.Components, who, true );
        }
        return report;
    }

    StepOutcome RunExposureBiasOnly( UpgradeContext& context )
    {
        ExposureKeyReport report = MigrateExposureKeyV77ToV78( context.Entities );
        StepOutcome       outcome;
        outcome.Changed = report.Folded;
        outcome.Summary = std::format( "{} AutoExposureKey value(s) folded into AutoExposureBias", report.Folded );
        outcome.Refused = std::move( report.Refused );
        return outcome;
    }
} // namespace Desert::Core
