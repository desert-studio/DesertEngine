#include <Engine/Core/SceneUpgrade/Steps/PostProcess1Bloom.hpp>

#include <Engine/Core/SceneUpgrade/Steps/StepEdits.hpp>

#include <format>
#include <optional>
#include <string>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        using UpgradeEdits::DropKey;
        using UpgradeEdits::EditBlock;
        using UpgradeEdits::NumberOf;

        // The step numbers, frozen: the post-process v0 PostProcessSettings defaults a record that did not state a
        // key rendered with (v1 states them).
        constexpr double kPostProcessV0BloomIntensity    = 0.8;
        constexpr double kPostProcessV0BloomThreshold    = 2.0;
        constexpr double kPostProcessV0AutoExposureSpeed = 1.5;
        // The histogram window every post-process v0 file was graded under (AutoExposureRenderer's constants
        // then).
        constexpr double kPostProcessV0LowPercent  = 0.0;
        constexpr double kPostProcessV0HighPercent = 95.0;
    } // namespace

    PostProcessBloomReport MigratePostProcessBloomV0ToV1( std::vector<Assets::EntityData>& entities )
    {
        PostProcessBloomReport report;

        // @p record: the entity's own block (absent keys meant the post-process v0 defaults, which this states);
        // an override is a delta, so only the keys it states are re-stated.
        const auto restate = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who, bool record )
        {
            EditBlock( components, "PostProcessVolume",
                       [&]( rfl::Generic::Object& block )
                       {
                           // A record without Settings had every post-process v0 default: it is restated from an
                           // empty object.
                           const auto           stated = block.get( "Settings" );
                           rfl::Generic::Object settings;
                           if ( stated.has_value() )
                           {
                               auto fields = stated.value().to_object();
                               if ( !fields.has_value() )
                               {
                                   report.Refused.push_back(
                                        std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                                     rfl::json::write( stated.value() ) ) );
                                   return false;
                               }
                               settings = std::move( fields.value() );
                           }
                           else if ( !record )
                               return false;
                           const auto number = [&]( const char*           key,
                                                    std::optional<double> fallback ) -> std::optional<double>
                           {
                               const auto value = settings.get( key );
                               if ( !value.has_value() )
                                   return fallback;
                               const auto parsed = NumberOf( value.value() );
                               if ( !parsed.has_value() )
                                   report.Refused.push_back(
                                        std::format( "{}: PostProcessVolume.Settings.{} {} is not a number", who,
                                                     key, rfl::json::write( value.value() ) ) );
                               return parsed;
                           };
                           const auto put = [&]( const char* key, rfl::Generic value )
                           {
                               DropKey( settings, key );
                               settings[key] = std::move( value );
                           };
                           const std::size_t refusedBefore = report.Refused.size();
                           bool              changed       = false;

                           // EnableBloom false -> Intensity 0 (UE has no switch); true keeps the intensity.
                           const auto          enable = settings.get( "EnableBloom" );
                           std::optional<bool> enabled;
                           if ( enable.has_value() )
                           {
                               const auto flag = enable.value().to_bool();
                               if ( !flag.has_value() )
                               {
                                   report.Refused.push_back(
                                        std::format( "{}: PostProcessVolume.Settings.EnableBloom {} is not a bool",
                                                     who, rfl::json::write( enable.value() ) ) );
                                   return false;
                               }
                               enabled = flag.value();
                           }
                           else if ( record )
                               enabled = false; // the post-process v0 default
                           if ( enabled.has_value() )
                           {
                               if ( !enabled.value() )
                               {
                                   put( "BloomIntensity", rfl::Generic( 0.0 ) );
                                   ++report.BloomOff;
                               }
                               else
                               {
                                   // An override switching bloom on over a base this step set to 0 states the
                                   // intensity it meant: its own, or the post-process v0 default.
                                   const auto intensity = number( "BloomIntensity", kPostProcessV0BloomIntensity );
                                   if ( intensity.has_value() )
                                       put( "BloomIntensity", rfl::Generic( intensity.value() ) );
                               }
                               DropKey( settings, "EnableBloom" );
                               changed = true;
                           }
                           // Threshold <= 0 bloomed every pixel: UE's -1. Above 0 the number is kept.
                           const auto threshold = number(
                                "BloomThreshold",
                                record ? std::optional<double>( kPostProcessV0BloomThreshold ) : std::nullopt );
                           if ( threshold.has_value() )
                           {
                               put( "BloomThreshold",
                                    rfl::Generic( threshold.value() <= 0.0 ? -1.0 : threshold.value() ) );
                               changed = true;
                           }
                           // One speed -> both directions at it.
                           const auto speed = number(
                                "AutoExposureSpeed",
                                record ? std::optional<double>( kPostProcessV0AutoExposureSpeed ) : std::nullopt );
                           if ( speed.has_value() )
                           {
                               DropKey( settings, "AutoExposureSpeed" );
                               put( "AutoExposureSpeedUp", rfl::Generic( speed.value() ) );
                               put( "AutoExposureSpeedDown", rfl::Generic( speed.value() ) );
                               changed = true;
                           }
                           if ( record )
                           {
                               // The former defaults where the record did not state them: auto exposure off and
                               // the 0 / 95 histogram window the scene was graded under.
                               if ( !settings.get( "AutoExposure" ).has_value() )
                                   put( "AutoExposure", rfl::Generic( false ) );
                               put( "AutoExposureLowPercent", rfl::Generic( kPostProcessV0LowPercent ) );
                               put( "AutoExposureHighPercent", rfl::Generic( kPostProcessV0HighPercent ) );
                               changed = true;
                           }
                           if ( report.Refused.size() != refusedBefore || !changed )
                               return false;
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
            restate( entity.Components, who, true );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    restate( override_.Components, who, false );
        }
        return report;
    }

    StepOutcome RunPostProcessBloom( UpgradeContext& context )
    {
        PostProcessBloomReport report = MigratePostProcessBloomV0ToV1( context.Entities );
        StepOutcome            outcome;
        outcome.Changed = report.Restated;
        outcome.Summary = std::format( "{} PostProcessVolume Settings block(s) restated as UE bloom and auto "
                                       "exposure, {} with bloom off as BloomIntensity 0",
                                       report.Restated, report.BloomOff );
        outcome.Refused = std::move( report.Refused );
        return outcome;
    }
} // namespace Desert::Core
