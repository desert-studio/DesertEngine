#include <Engine/Core/SceneUpgrade/Steps/V80WaterBodyRetiredKeys.hpp>

#include <Engine/Core/SceneUpgrade/Steps/StepEdits.hpp>

#include <array>
#include <format>
#include <string>

namespace Desert::Core
{
    namespace
    {
        using UpgradeEdits::DropKey;
        using UpgradeEdits::EditBlock;

        // The keys WaterBody v2 and v3 retired (V80WaterBodyRetiredKeys.hpp says what took over each).
        constexpr std::array kRetiredWaterBodyKeys{ "TargetWaveMaskDepth", "FallFoam" };
    } // namespace

    WaterBodyRetiredKeysReport MigrateWaterBodyRetiredKeysV79ToV80( std::vector<Assets::EntityData>& entities )
    {
        WaterBodyRetiredKeysReport report;

        const auto drop = [&]( rfl::ExtraFields<rfl::Generic>& components )
        {
            EditBlock( components, "WaterBody",
                       [&]( rfl::Generic::Object& block )
                       {
                           bool changed = false;
                           for ( const char* key : kRetiredWaterBodyKeys )
                               if ( DropKey( block, key ) )
                               {
                                   ++report.Dropped;
                                   changed = true;
                               }
                           return changed;
                       } );
        };

        for ( auto& entity : entities )
        {
            drop( entity.Components );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    drop( override_.Components );
        }
        return report;
    }

    StepOutcome RunWaterBodyRetiredKeys( UpgradeContext& context )
    {
        const WaterBodyRetiredKeysReport report = MigrateWaterBodyRetiredKeysV79ToV80( context.Entities );
        StepOutcome                      outcome;
        outcome.Changed = report.Dropped;
        outcome.Summary = std::format( "{} retired WaterBody key(s) dropped (TargetWaveMaskDepth: the depth "
                                       "transform derives it; FallFoam: the plunge boil)",
                                       report.Dropped );
        return outcome;
    }
} // namespace Desert::Core
