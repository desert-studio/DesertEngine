#pragma once

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Core/ResultStr.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    // THE ONE GENERATION OF .desce THIS ENGINE READS AND WRITES. The saver stamps it, the loader REQUIRES
    // it, and there is no second branch: a file that is not at this generation is refused by name and
    // nothing is created for it (see RefuseSceneVersion below).
    //
    // WHY THERE IS NO MIGRATION HERE. There used to be eight of them, one per schema step, and each was
    // written with a shelf life it never got: "deleted once no v<n> file remains". Eight accumulated steps
    // is eight expiries that were never enforced, and DEV_CONTRACT §4.3 - "the runtime knows nothing about
    // the old format" - had been false for every one of them. The steps were not deleted; they MOVED, whole,
    // to Tools/SceneMigrator, which is where a conversion belongs: it runs once, over files, and writes the
    // result back, which is the only thing that ever makes a migration expire (§4.6). An old file is
    // converted by one run of that tool instead of by every scene load of every build, forever.
    //
    // BUMPING THIS. Raising the number is one edit here plus one migration step in Tools/SceneMigrator, and
    // then a run of the tool over the repository - the run is not optional, because this loader will refuse
    // every scene that has not had it.
    //
    // AND IT IS NOT ONLY SCENES. A `.deprefab` carries the same Assets::EntityData and states THIS number
    // (Engine/Assets/Prefab/PrefabData.hpp), so raising it moves prefabs too. An entity-level step added to
    // Tools/SceneMigrator raises both, because both enter the same chain, and the one run of the tool over
    // the tree converts both, because it collects both extensions. That is the whole of what И11 had to
    // build: before it, prefabs shared this number with a migrator that had no steps, so a bump left every
    // existing prefab at the old generation with nothing able to read it.
    //
    // AND IT IS WHAT MAKES RETIREMENT WORK. Since K11 a key this build does not declare is PRESERVED across
    // a save (Serialize/ForeignKeys.hpp) - so the only thing that can ever remove one is a deliberate
    // decision, and the only place that decision can be written down is a migration step, which names the
    // key, drops it from the FILES, and is made compulsory by this number moving. The loader therefore
    // needs no list of dead keys and must never grow one: "retired" is a fact about a conversion that has
    // already happened, not a rule the runtime carries.
    inline constexpr int kSceneVersion = 30;

    // World-unit generation of a .desce file. One world unit is a CENTIMETRE (Common/Core/Units.hpp).
    // Bump this only if the world unit changes again - and then, as above, add the step to SceneMigrator
    // and run it, because nothing converts a file at load any more.
    inline constexpr int kUnitVersion = 1;

    // THIS WORLD IS PARTITIONED, AND THE NUMBERS THAT CANNOT BE DERIVED.
    //
    // PRESENCE IS THE SWITCH. A world is partitioned or it is not, and the absence of this block is how a
    // file says "not". There is no `Enabled` flag, because a flag would make `{"Enabled":false}` and no
    // block at all two spellings of one state - and the moment a state has two spellings, the two drift.
    // It is a `std::optional` member below for the same reason the mesh mirrors' `CastShadows` is one:
    // reflect-cpp omits a `nullopt` field entirely, so none of the `.desce` files in this repository
    // changes a single byte. The suite Desert/Tests/Engine/WorldPartition asserts that over the corpus.
    //
    // WHY IT IS A PROPERTY OF THE WORLD AND NOT AN ENGINE SETTING: owner decision 2026-09-18 - a world is
    // partitioned or it is not, and this is never an engine mode. A scene of twenty-four entities must not
    // pay for a grid.
    //
    // WHAT A HUMAN STATES, AND WHAT IS DERIVED. Cell boundaries, grid LEVELS, which level and cell a
    // composite is in, and what is always loaded are all functions of the scene's own contents (owner
    // decisions 2026-09-18 and O1 2026-09-23; Rules::PlanWorldPartition). What cannot be derived is how
    // big a cell is and how far away one starts to load - per grid.
    //
    // THE GRID IS ONE ENTRY OF A LIST FROM THE FIRST FORMAT THAT HAS IT (A7). One grid is used today;
    // several - per-record grid choice, e.g. a coarse grid for distant landmarks - are WP22, and a list
    // now means that step adds entries rather than reshaping the block.
    //
    // DATA LAYERS HAVE A PLACE, AND NOT A FIELD (O4: deferred). They will be a SIBLING of `Grids` in this
    // object - `DataLayers`, a list of named layers - plus a membership list on the record, and the cell a
    // composite lands in gains the layer as a third coordinate beside level and square. Every piece of
    // that is ADDITIVE to this shape, which is the reservation: nothing here has to move for it. It is not
    // an empty field today because an empty field is a setting nothing reads, and because what it would
    // not buy is the one real hazard - an OLDER build reading a newer file and loading every layer at
    // once. That is closed the way every schema step is closed: the step that adds `DataLayers` raises
    // kSceneVersion, and an older build refuses the file by name (SceneIsAtCurrentVersion).
    struct WorldPartitionGridSerialized
    {
        // Edge length of one level-0 cell in WORLD UNITS, and a world unit is a CENTIMETRE
        // (Common/Core/Units.hpp). Level L cells are CellSize * 2^L. The default is UE's own main grid,
        // 128 m, in our units; it is the number a world gets when switched on, nothing depends on it.
        //
        // CELLS ARE SQUARE AND THE GRID IS TWO-DIMENSIONAL - X and Z, unbounded in Y. That is the PATTERN
        // of UE's grid and not its letter (owner, 2026-09-22): the acceptance criterion is running ACROSS a
        // map, so the axis a player leaves behind is horizontal.
        float CellSize = 12800.0f;

        // How far from a streaming source a cell of this grid starts to load, in world units. UE's main
        // grid default, 256 m. Read by the streaming query (WP4); the partition itself does not depend
        // on it, which is why a cell's contents never change when it is edited.
        float LoadingRange = 25600.0f;
    };

    struct WorldPartitionSerialized
    {
        std::vector<WorldPartitionGridSerialized> Grids;
    };

    // The on-disk shape of a .desce file, and the ONLY definition of it: the loader parses into this, the
    // saver writes it, and Tools/SceneMigrator rewrites it in place. It lives here rather than inside
    // SceneSerializer.cpp because a migration whose input is "the parsed tree" needs the tree's type, and a
    // second copy of this struct anywhere is a format that can silently fork.
    struct SceneSerialized
    {
        // First, so it is the document's first member and readable without the body (TextAssetHeader.hpp).
        // It is where the file states its two generations (SCNE, UNIT) - since v26 nowhere else.
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        std::string                                               SceneName;
        std::vector<Assets::EntityData> Entities;
        // Scene-wide settings - reflected, so the whole block round-trips through the generic serializer.
        std::optional<rfl::Generic> Settings;
        // Absent = this world is not partitioned. See WorldPartitionSerialized.
        std::optional<WorldPartitionSerialized> WorldPartition;
    };

    // True when the parsed tree is at BOTH current generations, which is the only thing the loader accepts.
    //
    // An ABSENT integer is version 0 and not "current": every stamp this engine writes states both numbers,
    // so a file missing one was written by something older. Reading it as current is the substitution §1.4
    // forbids - it would load a metres-era scene as though it were centimetres and put the world a hundred
    // times too small on screen with nothing said about it.
    [[nodiscard]] inline bool SceneIsAtCurrentVersion( const SceneSerialized& scene )
    {
        return Assets::StatedVersion( scene.Header, Assets::kSceneSchemaTag ) == kSceneVersion &&
               Assets::StatedVersion( scene.Header, Assets::kUnitSchemaTag ) == kUnitVersion;
    }

    // The subsystem versions a .desce / .deprefab of this build states, and the reading context built on them.
    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> SceneTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 2> versions = {
             Common::Content::SubsystemVersion{ Assets::kSceneSchemaTag, static_cast<uint32_t>( kSceneVersion ) },
             Common::Content::SubsystemVersion{ Assets::kUnitSchemaTag, static_cast<uint32_t>( kUnitVersion ) } };
        return versions;
    }

    // The refusal, as a string: which file, what it is, what this engine needs, and the exact command that
    // fixes it. PURE - no logging, no filesystem, no globals - so the loader can log it and a test can read
    // it without a GPU or a scene.
    //
    // It names the COMMAND and not just the tool, because "your scene is too old" without the fix is a dead
    // end for whoever hits it: the file is usually somebody's autosave or a scene saved outside the
    // repository, and they have one action to take.
    [[nodiscard]] std::string RefuseSceneVersion( std::string_view source, int foundSceneVersion,
                                                  int foundUnitVersion );

    // Same, read straight off the parsed tree (absent integers become 0 - see SceneIsAtCurrentVersion).
    [[nodiscard]] std::string RefuseSceneVersion( std::string_view source, const SceneSerialized& scene );

    // Reads the JSON of a .desce and hands back the tree ONLY if this engine will load it: readable, and at
    // both current generations. Otherwise the error is the message the loader logs, already naming the file,
    // what it is, what is needed and the command that converts it.
    //
    // WHY THIS IS SEPARATE FROM THE LOADER, AND WHY CALLERS ASK IT FIRST. Opening a scene DESTROYS the one
    // that is open - every caller of SceneSerializer::DeserializeFromJson clears the scene before it hands
    // the text over. If the refusal only happened inside the loader, opening an old file would empty the
    // editor and then decline to fill it, which is a worse answer than the migration this replaced: the
    // user would lose the scene they had to a file that never loaded. So the question "will this load?" is
    // answerable without a Scene, and the callers ask it while there is still something to protect.
    //
    // PURE - no filesystem (the caller has already read the text), no globals, no scene. The parse it does
    // is the same one the loader does; the loader calls THIS rather than repeating the checks, so there is
    // one statement of what "loadable" means and one wording of the refusal.
    [[nodiscard]] Common::ResultStr<SceneSerialized> ParseLoadableScene( std::string_view   source,
                                                                         const std::string& json );

} // namespace Desert::Core
