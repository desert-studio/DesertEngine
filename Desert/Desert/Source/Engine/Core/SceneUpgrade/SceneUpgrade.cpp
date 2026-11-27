#include <Engine/Core/SceneUpgrade/SceneUpgrade.hpp>

#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/AssetHandle.hpp>

#include <filesystem>
#include <format>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        // A file refused over a step's list, in one line naming the file and every refusal.
        std::string RefusedWhole( std::string_view name, const std::vector<std::string>& refusals )
        {
            std::string joined;
            for ( const std::string& refusal : refusals )
                joined += ( joined.empty() ? std::string() : std::string( "; " ) ) + refusal;
            return std::format( "'{}': {}", name, joined );
        }

        std::string GenerationsText( const SceneGenerations& g )
        {
            return std::format( "scene v{} / world units v{} / world partition v{} / asset references v{} / "
                                "lights v{} / post process v{}",
                                g.Scene, g.Unit, g.WorldPartition, g.AssetRef, g.Light, g.PostProcess );
        }

        // The number @p generations states for @p chain.
        int StatedOn( const SceneGenerations& generations, const UpgradeChain chain )
        {
            switch ( chain )
            {
            case UpgradeChain::Scene:
                return generations.Scene;
            case UpgradeChain::WorldPartition:
                return generations.WorldPartition;
            case UpgradeChain::AssetRef:
                return generations.AssetRef;
            case UpgradeChain::Light:
                return generations.Light;
            case UpgradeChain::PostProcess:
                return generations.PostProcess;
            }
            return 0;
        }
    } // namespace

    MaterialGuidLookup ContentMaterialGuidLookup()
    {
        return []( const std::string& key ) -> std::optional<std::string>
        {
            const std::filesystem::path file = Common::AssetHandle::PathForStableKey( key );
            if ( file.empty() )
                return std::nullopt;
            const Common::Content::AssetGuid guid = Assets::ReadTextHeaderGuid( file );
            if ( guid == Common::Content::AssetGuid{} )
                return std::nullopt;
            return Common::Content::AssetGuidToText( guid );
        };
    }

    SceneGenerations StatedGenerations( const std::optional<Common::Content::TextAssetHeaderSerialized>& header )
    {
        return SceneGenerations{ Assets::StatedVersion( header, Assets::kSceneSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kUnitSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kWorldPartitionSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kAssetRefSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kLightSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kPostProcessSchemaTag ) };
    }

    SceneGenerations CurrentGenerations()
    {
        return SceneGenerations{ kSceneVersion,    kUnitVersion,  kWorldPartitionVersion,
                                 kAssetRefVersion, kLightVersion, kPostProcessVersion };
    }

    SceneUpgradeReport UpgradeSceneRecords( std::vector<Assets::EntityData>& entities, const SceneGenerations& from,
                                            const UpgradeTarget target, const std::string_view name,
                                            const MaterialGuidLookup& materialGuid )
    {
        SceneUpgradeReport report;
        report.From                 = from;
        const SceneGenerations head = CurrentGenerations();

        // NEWER than this engine: written by a later build. Nothing is read "as best we can".
        if ( from.Scene > head.Scene || from.Unit > head.Unit || from.WorldPartition > head.WorldPartition ||
             from.AssetRef > head.AssetRef || from.Light > head.Light || from.PostProcess > head.PostProcess )
        {
            report.Refused = std::format(
                 "'{}' is at {}, NEWER than this engine ({}): it was written by a later build. Open it with that "
                 "build or a newer one.",
                 name, GenerationsText( from ), GenerationsText( head ) );
            return report;
        }
        // OLDER than the oldest generation raised in memory: the steps below the floors move data between files.
        const SceneGenerations floor{ kSceneUpgradeFloor,    kUnitVersion,       kWorldPartitionUpgradeFloor,
                                      kAssetRefUpgradeFloor, kLightUpgradeFloor, kPostProcessUpgradeFloor };
        if ( from.Scene < floor.Scene || from.Unit != head.Unit || from.WorldPartition < floor.WorldPartition ||
             from.AssetRef < floor.AssetRef || from.Light < floor.Light || from.PostProcess < floor.PostProcess )
        {
            report.Refused = std::format(
                 "'{}' is at {}, older than the oldest generation this engine upgrades as it loads ({}). Its steps "
                 "write other files beside it, so it is converted once by Tools/SceneMigrator, which writes the "
                 "current generation.",
                 name, GenerationsText( from ), GenerationsText( floor ) );
            return report;
        }

        // The table, in order: a row runs when its chain's stated number is below the row's.
        UpgradeContext context{ entities, target, name, materialGuid };
        for ( const UpgradeStep& step : UpgradeSteps() )
        {
            if ( StatedOn( from, step.Chain ) >= step.To )
                continue;
            StepOutcome outcome = step.Run( context );
            outcome.Chain       = step.Chain;
            outcome.To          = step.To;
            outcome.Name        = step.Name;
            const bool refused  = !outcome.Refused.empty();
            if ( refused )
                report.Refused = RefusedWhole( name, outcome.Refused );
            report.Steps.push_back( std::move( outcome ) );
            if ( refused )
                return report;
        }
        return report;
    }

    void RestateCurrentGenerations( Common::Content::TextAssetHeaderSerialized& header )
    {
        header.Versions = Common::Content::MakeTextHeader( Common::Content::ContentKind::Scene,
                                                           Common::Content::AssetGuid{}, SceneTextSubsystems() )
                               .Versions;
    }

    SceneUpgradeReport UpgradeScene( SceneSerialized& scene )
    {
        SceneGenerations from = StatedGenerations( scene.Header );
        // World Partition below its floor: every step under v12 (Tools/SceneMigrator, RunWorldPartitionSteps) reads
        // the scene's WorldPartition block and nothing else - grids, HLOD layers, partitions. A scene that states no
        // block (GI_Bistro_Night, WPAR 1) has nothing for them to move, so it starts its chain at the floor instead
        // of being refused for a world it does not have.
        if ( !scene.WorldPartition.has_value() && from.WorldPartition < kWorldPartitionUpgradeFloor )
            from.WorldPartition = kWorldPartitionUpgradeFloor;
        if ( from == CurrentGenerations() )
        {
            SceneUpgradeReport current;
            current.From = from;
            return current;
        }
        SceneUpgradeReport report =
             UpgradeSceneRecords( scene.Entities, from, UpgradeTarget::Scene, scene.SceneName,
                                  ContentMaterialGuidLookup() );
        if ( report.Refused.empty() && scene.Header.has_value() )
            RestateCurrentGenerations( *scene.Header );
        return report;
    }

    std::string DescribeUpgrade( const std::string_view source, const SceneUpgradeReport& report )
    {
        const SceneGenerations head = CurrentGenerations();
        // The scene number leads ("upgraded v71->v78 in memory"); a subsystem that moved too, then the steps that
        // ran, follow in brackets.
        std::string detail;
        const auto  add = [&]( const std::string& part )
        { detail += ( detail.empty() ? std::string() : std::string( "; " ) ) + part; };
        if ( report.From.WorldPartition != head.WorldPartition )
            add( std::format( "world partition v{}->v{}", report.From.WorldPartition, head.WorldPartition ) );
        if ( report.From.AssetRef != head.AssetRef )
            add( std::format( "asset references v{}->v{}", report.From.AssetRef, head.AssetRef ) );
        if ( report.From.Light != head.Light )
            add( std::format( "lights v{}->v{}", report.From.Light, head.Light ) );
        if ( report.From.PostProcess != head.PostProcess )
            add( std::format( "post process v{}->v{}", report.From.PostProcess, head.PostProcess ) );
        std::string steps;
        for ( const StepOutcome& step : report.Steps )
            steps += ( steps.empty() ? std::string() : std::string( ", " ) ) + std::string( step.Name );
        if ( !steps.empty() )
            add( "steps " + steps );
        return std::format( "'{}': upgraded v{}->v{} in memory ({}), save to keep", source, report.From.Scene,
                            head.Scene, detail );
    }
} // namespace Desert::Core
