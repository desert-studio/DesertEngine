#pragma once

#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// THE STEPS OF THE SCENE UPGRADE AS DATA (a migration is a unit: Flyway / Rails migrations, UE FCustomVersion
// fixups). Every step is one file under Steps/ - its frozen version number, its typed report, its pure Migrate
// function and the Run adapter below - and one row of the ordered table UpgradeSteps() returns
// (SceneUpgradeSteps.cpp). The driver (UpgradeSceneRecords, SceneUpgrade.cpp) walks the table; it names no step.
namespace Desert::Core
{
    // The header GUID (text) of the material an asset key ("assets:Materials/M.demat") names, or nullopt when
    // that file states none. Where the file is read from is the caller's: a step never opens a file.
    using MaterialGuidLookup = std::function<std::optional<std::string>( const std::string& key )>;

    enum class UpgradeTarget
    {
        Scene,  // a spline becomes its own entity
        Prefab, // a spline is refused: it cannot become an entity of an instance
    };

    // The version tag a step raises; the table runs the chains in this order.
    enum class UpgradeChain
    {
        Scene,          // Assets::kSceneSchemaTag
        WorldPartition, // Assets::kWorldPartitionSchemaTag
        AssetRef,       // Assets::kAssetRefSchemaTag
        Light,          // Assets::kLightSchemaTag
        PostProcess,    // Assets::kPostProcessSchemaTag
    };

    [[nodiscard]] constexpr std::string_view UpgradeChainName( const UpgradeChain chain )
    {
        switch ( chain )
        {
        case UpgradeChain::Scene:
            return "scene";
        case UpgradeChain::WorldPartition:
            return "world partition";
        case UpgradeChain::AssetRef:
            return "asset references";
        case UpgradeChain::Light:
            return "lights";
        case UpgradeChain::PostProcess:
            return "post process";
        }
        return "unknown chain";
    }

    // What one step is given: the records of one file and what the file is.
    struct UpgradeContext
    {
        std::vector<Assets::EntityData>& Entities;
        UpgradeTarget                    Target;
        std::string_view                 Name;         // names the file in a refusal
        const MaterialGuidLookup&        MaterialGuid; // answers the asset-reference step
    };

    // What one step did. A step's Run states Changed, Summary and Refused; the driver stamps Chain, To and Name
    // from the table row, so a step's identity has one source.
    struct StepOutcome
    {
        UpgradeChain             Chain = UpgradeChain::Scene;
        int                      To    = 0;
        std::string_view         Name;        // the table row's name (static storage)
        std::size_t              Changed = 0; // values the step restated, added or removed
        std::string              Summary;     // one human line: what the step did to this file
        std::vector<std::string> Refused;     // non-empty: the file is refused, the chain stops here
    };

    // One row of the chain: a step that raises @p Chain to @p To.
    struct UpgradeStep
    {
        UpgradeChain     Chain;
        int              To;
        std::string_view Name;
        StepOutcome ( *Run )( UpgradeContext& );
    };

    // THE CHAIN, ordered: every scene step, then World Partition's, the asset-reference chain's, the light
    // chain's, the post-process chain's - each chain's rows contiguous from its floor + 1 to its head (static_asserts in
    // SceneUpgradeSteps.cpp). No registration, no singleton: a constant array.
    [[nodiscard]] std::span<const UpgradeStep> UpgradeSteps();
} // namespace Desert::Core
