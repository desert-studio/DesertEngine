#pragma once

#include <Engine/Assets/Prefab/PrefabData.hpp>

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
    inline constexpr int kSceneVersion = 20;

    // World-unit generation of a .desce file. One world unit is a CENTIMETRE (Common/Core/Units.hpp).
    // Bump this only if the world unit changes again - and then, as above, add the step to SceneMigrator
    // and run it, because nothing converts a file at load any more.
    inline constexpr int kUnitVersion = 1;

    // The on-disk shape of a .desce file, and the ONLY definition of it: the loader parses into this, the
    // saver writes it, and Tools/SceneMigrator rewrites it in place. It lives here rather than inside
    // SceneSerializer.cpp because a migration whose input is "the parsed tree" needs the tree's type, and a
    // second copy of this struct anywhere is a format that can silently fork.
    struct SceneSerialized
    {
        std::string                     SceneName;
        std::vector<Assets::EntityData> Entities;
        // Scene-wide settings - reflected, so the whole block round-trips through the generic serializer.
        std::optional<rfl::Generic> Settings;
        std::optional<int>          UnitVersion;
        std::optional<int>          SceneVersion;
    };

    // True when the parsed tree is at BOTH current generations, which is the only thing the loader accepts.
    //
    // An ABSENT integer is version 0 and not "current": every stamp this engine writes states both numbers,
    // so a file missing one was written by something older. Reading it as current is the substitution §1.4
    // forbids - it would load a metres-era scene as though it were centimetres and put the world a hundred
    // times too small on screen with nothing said about it.
    [[nodiscard]] inline bool SceneIsAtCurrentVersion( const SceneSerialized& scene )
    {
        return scene.SceneVersion.value_or( 0 ) == kSceneVersion &&
               scene.UnitVersion.value_or( 0 ) == kUnitVersion;
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
