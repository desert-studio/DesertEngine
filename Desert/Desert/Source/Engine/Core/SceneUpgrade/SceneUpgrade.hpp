#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// UPGRADE ON LOAD (SCENE-UPGRADE-ON-LOAD; UE: FCustomVersion + Serialize upgrading an older package as it loads,
// ResavePackages running the very same code to write it back). A .desce / .deprefab OLDER than this engine is
// raised IN MEMORY by the steps of this folder when it is parsed (Core::ParseLoadableScene,
// Assets::ParseLoadablePrefab), and the next save writes the current generation - the file on disk is untouched
// until somebody saves it. Tools/SceneMigrator is the resave: it reads, calls THESE functions, and writes; it
// carries no copy of them.
//
// THE OLDEST LOADABLE GENERATION (UE: VER_UE4_OLDEST_LOADABLE_PACKAGE). The steps below kSceneUpgradeFloor move
// data between FILES (prefab files, landscape tiles, mesh headers, .defoliage / .desound / .vfx assets minted
// beside the scene) or read shapes the engine's tree no longer has a member for; they stay in
// Tools/SceneMigrator, and a file below the floor is refused by name with that one command. Every step at or
// above a floor - the scene's v71.., World Partition's v13.., the asset-reference, light and post-process chains
// whole - is in Steps/ and only there: the migrator runs its below-floor chain, then this one
// (UpgradeSceneRecords).
//
// A STEP THAT NEEDS ANOTHER FILE'S CONTENT DOES NOT OPEN IT. The asset-reference step reads a material's header
// GUID through the MaterialGuidLookup its caller passes: the loader's reads the mounted content
// (ContentMaterialGuidLookup), the migrator's its own assets root. The transform is one copy.
//
// SELF-VERSIONED BLOCKS upgrade in their own reader, not here (UE: a struct's Serialize reads its own custom
// version): a timeline block states its TMLN number inside itself, so a TMLN v1 block - in a scene's UIAnim, a
// .dseq, an .anim - is raised by the one reader every host goes through (Animation::Timeline::ReadSequence,
// TimelineInterpShift.hpp). A file NEWER than this engine (any tag above the head) is refused as before: nothing
// is ever loaded "as best we can".
//
// ADDING A STEP. A change that belongs to one subsystem raises THAT subsystem's own number, as UE counts a
// subsystem's changes under its own FCustomVersion (MIGRATE-SUBSYS, owner decision 10-22): the lights'
// Core::kLightVersion, the post-process settings' Core::kPostProcessVersion, the asset references', World
// Partition's - and a subsystem that has none yet gets its own tag (Assets/TextAssetHeaderStamp.hpp), its number
// in SceneFormat.hpp, a field in SceneGenerations and an UpgradeChain, never a bump of the shared scene number.
// Only a change to the scene's own shape raises Core::kSceneVersion. Either way: add one file pair under Steps/
// (V<N><Name>, or <Subsystem><N><Name>) - its frozen k<Subsystem>Version<Name>, its report, its Migrate function
// and its Run adapter - and one row at the end of its chain in SceneUpgradeSteps.cpp. Nothing else: the loader, the prefab reader and the migrator all reach it
// through UpgradeSceneRecords, which walks that table.
namespace Desert::Core
{
    //  70 - the floor: the oldest scene generation this engine raises in memory (kSceneUpgradeFloor). The steps
    //       up to it are Tools/SceneMigrator's (MigrateRootSiblingIndexV69ToV70 the last).
    inline constexpr int kSceneVersionRootSiblingIndex = 70;

    // World Partition's own chain (Assets::kWorldPartitionSchemaTag). v12 is the floor (the last step that moves a
    // value, MigrateHLODMergeMaterialsV11ToV12, is Tools/SceneMigrator's).
    inline constexpr int kWorldPartitionVersionHLODBuilderSettings = 12;

    // THE FLOORS: the oldest generation of each tag this engine raises in memory. The asset-reference, light and
    // post-process chains are here whole (floor 0: a file that states no number).
    inline constexpr int kSceneUpgradeFloor          = kSceneVersionRootSiblingIndex;
    inline constexpr int kWorldPartitionUpgradeFloor = kWorldPartitionVersionHLODBuilderSettings;
    inline constexpr int kAssetRefUpgradeFloor       = 0;
    inline constexpr int kLightUpgradeFloor          = 0;
    inline constexpr int kPostProcessUpgradeFloor    = 0;

    [[nodiscard]] constexpr int UpgradeFloorOf( const UpgradeChain chain )
    {
        switch ( chain )
        {
        case UpgradeChain::Scene:
            return kSceneUpgradeFloor;
        case UpgradeChain::WorldPartition:
            return kWorldPartitionUpgradeFloor;
        case UpgradeChain::AssetRef:
            return kAssetRefUpgradeFloor;
        case UpgradeChain::Light:
            return kLightUpgradeFloor;
        case UpgradeChain::PostProcess:
            return kPostProcessUpgradeFloor;
        }
        return 0;
    }

    // The loader's lookup: the key through the mounted content roots (Common::AssetHandle::PathForStableKey),
    // the header read VFS-first (Assets::ReadTextHeaderGuid) - the identity every load of that material adopts.
    [[nodiscard]] MaterialGuidLookup ContentMaterialGuidLookup();

    // The generations a file states (absent = 0), one per tag.
    struct SceneGenerations
    {
        int Scene          = 0;
        int Unit           = 0;
        int WorldPartition = 0;
        int AssetRef       = 0;
        int Light          = 0;
        int PostProcess    = 0;

        [[nodiscard]] bool operator==( const SceneGenerations& ) const = default;
    };
    [[nodiscard]] SceneGenerations
    StatedGenerations( const std::optional<Common::Content::TextAssetHeaderSerialized>& header );
    // This build's generations (the head of every tag).
    [[nodiscard]] SceneGenerations CurrentGenerations();

    // What one upgrade did: every step that ran, in table order. Refused non-empty: the records are in an
    // unspecified state and the caller loads and writes nothing (the refusing step is the last of Steps).
    struct SceneUpgradeReport
    {
        SceneGenerations         From;
        std::vector<StepOutcome> Steps;
        std::string              Refused;

        [[nodiscard]] bool AnyRaised() const { return !Steps.empty(); }
    };

    // THE CHAIN, over the records of one file whose generations are @p from. Every tag must be at or above its
    // floor and at or below its head (else Refused names the numbers); then every row of UpgradeSteps() whose
    // number is above its chain's stated one runs, in order, until one refuses. Does not touch a header: the
    // caller restates it (RestateCurrentGenerations) or, for the migrator, stamps it. @p name names the file in a
    // refusal; @p materialGuid answers the asset-reference step (asked only when the file states asset references
    // v0). PURE given it.
    [[nodiscard]] SceneUpgradeReport UpgradeSceneRecords( std::vector<Assets::EntityData>& entities,
                                                          const SceneGenerations& from, UpgradeTarget target,
                                                          std::string_view          name,
                                                          const MaterialGuidLookup& materialGuid );

    // The header's Versions become this build's (Kind, Guid and Dependencies are kept): what an upgraded tree
    // states, so every later check of it reads current.
    void RestateCurrentGenerations( Common::Content::TextAssetHeaderSerialized& header );

    // A scene read by the engine: the chain from its header's numbers, then the header restated. A tree already
    // current is left untouched (AnyRaised false, From == CurrentGenerations()). Materials are looked up
    // through ContentMaterialGuidLookup.
    [[nodiscard]] SceneUpgradeReport UpgradeScene( SceneSerialized& scene );

    // The one log line of an in-memory upgrade:
    // "'<source>': upgraded v71->v78 in memory (world partition v12->v14; steps VolumetricFog, ...), save to keep".
    [[nodiscard]] std::string DescribeUpgrade( std::string_view source, const SceneUpgradeReport& report );
} // namespace Desert::Core
