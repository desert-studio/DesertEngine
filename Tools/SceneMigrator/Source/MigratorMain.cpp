// SceneMigrator — raises every .desce it is pointed at to the current scene generation and writes it back.
//
// WHY THIS EXISTS. The engine has always migrated old scenes on LOAD, and never once written the result
// down. Nothing in the repository carried a UnitVersion, so every load of every scene re-ran the
// metres-to-centimetres migration, the files stayed permanently authored in metres, and a scene authored
// correctly in world units was silently multiplied by a hundred the first time anyone opened it. The
// contract's migration clause (DEV_CONTRACT §4.3/§4.5) says data migrates once and is written back in the
// new form, and that "the scenes in the repository are converted by the same task". This is the thing that
// converts them.
//
// AND IT IS NOW THE ONLY THING THAT MIGRATES. The migrations used to ALSO live in the engine and run on
// every scene load; they are Source/SceneMigration.cpp beside this file now, and Core::kSceneVersion is a
// requirement the loader enforces rather than a target it drags files towards. So this tool is not a
// convenience any more — it is the conversion, and the loader's refusal names it by command line.
//
// It parses into Core::SceneSerialized, the engine's own struct for the current on-disk shape, and writes
// the same tree back out, so the file this produces is the file the engine reads. There is no second
// statement of the format to disagree with the first.
//
// It needs no GPU, no asset manager and no scene graph, because the migrations are pure functions over the
// parsed tree. That is what makes running it over a whole repository safe: a scene whose meshes or
// materials cannot be resolved on this machine still round-trips exactly, because nothing here resolves
// them.
//
// AND IT RAISES `.demat` FILES TOO, since O-4. The cloud LOOK has lived in a material rather than in the
// scene since v12, so a tool that migrated only scenes would leave every cloud material behind — carrying,
// in that case, a layout slot the shader no longer declares. A `.demat` has no version field, so those
// steps are content-detected and idempotent rather than version-gated; see
// MigrateCloudMaterialLayoutInputs and MigrateCloudMaterialAlbedoToColour.
//
// TWO STEPS NOW, AND NEITHER GATES THE OTHER. The second raises a scalar `ScatteringAlbedo` to a neutral
// colour, which is DATA LOSS if it is skipped rather than a missing feature: a scalar stored as
// (x, 0, 0, 0) and read as a colour is a medium that scatters red and absorbs green and blue outright.
// A material can need it without ever having had a layout binding, so both reports are consulted before
// the file is called clean.
//
// AND `.deprefab` FILES, since И11, and this is why there is no second tool. A prefab carries the scene's
// own EntityData and SHARES the scene's two version integers, so raising the head moves prefabs too — but
// the conversion used to live in a separate binary with a separate corpus, so raising the head and running
// only this one left every prefab in the tree at the old number, refused by the loader and by its own
// migrator alike. One number, one chain (Source/SceneMigration.cpp), one command.
//
//   SceneMigrator <path>...          .desce, .demat and .deprefab files, or directories searched
//                                    recursively
//   SceneMigrator --check <path>...  report what would change and write nothing (exit 1 if any would)

#include "MigratorMain.hpp"
#include "SceneMigration.hpp"
#include "SettingsCanonical.hpp"

#include <Common/Content/CanonicalText.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipMigrate.hpp>

#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr const char* kSceneExtension = ".desce";

    // MATERIALS ARE COLLECTED TOO, since O-4. A `.demat` has no version field to gate on, so the
    // material step is content-detected (see MigrateCloudMaterialLayoutInputs) — but it still has to be
    // REACHED, and the cloud look has lived in `.demat` files rather than in scenes since v12. A tool
    // that migrated only the scenes would leave every cloud material behind with a slot the shader no
    // longer declares.
    constexpr const char* kMaterialExtension = ".demat";

    // PREFABS ARE COLLECTED TOO, since И11, and by THIS tool rather than a second one. A `.deprefab`
    // carries the scene's own EntityData and shares the scene's two version integers, so it moves
    // through the same generations and is raised by the same chain — and the two-binaries arrangement it
    // replaces meant that raising the head required remembering to run a SECOND command over a SECOND
    // corpus. Forgetting it is precisely the data loss the shared version number creates: prefabs left
    // at the old number, refused by the loader and by their own migrator alike.
    constexpr const char* kPrefabExtension = ".deprefab";

    // CLIPS ARE COLLECTED TOO, since A5, and by THIS tool for the reason the prefabs gave: a second
    // binary over a second corpus is a command somebody forgets, and forgetting it leaves files the
    // loader refuses. A `.anim` does NOT share the scene's version integers — it has its own sequence,
    // because a scene names a clip by NAME and nothing in a `.desce` changes when a clip's time model
    // does. Its step is gated on its own number and is content-detected the same way: a file with no
    // version field is generation 0, never "already current".
    constexpr const char* kClipExtension = ".anim";

    void Collect( const std::filesystem::path& root, std::vector<std::filesystem::path>& scenes,
                  std::vector<std::filesystem::path>& materials, std::vector<std::filesystem::path>& prefabs,
                  std::vector<std::filesystem::path>& clips )
    {
        std::error_code ec;
        if ( std::filesystem::is_directory( root, ec ) )
        {
            for ( const auto& entry : std::filesystem::recursive_directory_iterator( root, ec ) )
            {
                if ( !entry.is_regular_file() )
                    continue;
                if ( entry.path().extension() == kSceneExtension )
                    scenes.push_back( entry.path() );
                else if ( entry.path().extension() == kMaterialExtension )
                    materials.push_back( entry.path() );
                else if ( entry.path().extension() == kPrefabExtension )
                    prefabs.push_back( entry.path() );
                else if ( entry.path().extension() == kClipExtension )
                {
                    clips.push_back( entry.path() );
                }
            }
            return;
        }

        if ( root.extension() == kMaterialExtension )
            materials.push_back( root );
        else if ( root.extension() == kPrefabExtension )
            prefabs.push_back( root );
        else if ( root.extension() == kClipExtension )
        {
            clips.push_back( root );
        }
        else
            scenes.push_back( root );
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Every text asset this tool writes goes through the one canonical writer (AF6), so a migrated file and a
    // file saved by the engine are laid out alike and a migration's diff is only the lines it changed.
    bool WriteText( const std::filesystem::path& path, const std::string& json, std::ostream& err )
    {
        const auto text = Common::Content::CanonicalJsonText( json );
        if ( !text )
        {
            err << "FAIL   " << path.string() << " — " << text.GetError() << "\n";
            return false;
        }
        const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, text.GetValue() );
        if ( !written )
            err << "FAIL   " << path.string() << " — " << written.GetError() << "\n";
        return static_cast<bool>( written );
    }

    // A file whose DOCUMENT is current but whose TEXT is not canonical is re-laid-out, not raised: its version
    // stays, because the content is unchanged (the CanonicalText suite proves old and new parse to one document).
    enum class Layout
    {
        Canonical,
        Relaid,
        Failed,
    };

    Layout RelayOutIfNeeded( const std::filesystem::path& path, const std::string& source, bool check,
                             std::ostream& out, std::ostream& err )
    {
        if ( Common::Content::IsCanonicalJsonText( source ) )
            return Layout::Canonical;
        if ( check )
        {
            out << "WOULD  " << path.string() << " — text layout only (canonical text), version unchanged\n";
            return Layout::Relaid;
        }
        if ( !WriteText( path, source, err ) )
        {
            err << "FAIL   " << path.string() << " — the layout could not be written; the original file is "
                << "untouched\n";
            return Layout::Failed;
        }
        out << "relaid " << path.string() << " — text layout only (canonical text), version unchanged\n";
        return Layout::Relaid;
    }

    // THE `.demat` FILES A v11 -> v12 RAISE PRODUCED, written FIRST and atomically, before the file that
    // names them: a file naming a material which does not exist is worse than one not yet migrated, so if
    // a material cannot be written its source is not either. Returns false on failure, having named it,
    // and the caller then leaves the source file exactly as it found it.
    //
    // Shared by scenes and prefabs since И11 — a prefab can carry a VolumetricCloud entity like any other,
    // and the collision guard below has to see BOTH file classes through ONE `claimed` map, or two files
    // of different classes minting the same material name would overwrite each other unseen.
    bool WriteCloudMaterials( const std::vector<Desert::Migration::CloudMaterialFile>& produced,
                              const std::filesystem::path& assetsRoot, const std::filesystem::path& source,
                              std::map<std::string, std::string>& claimed, std::ostream& out, std::ostream& err )
    {
        for ( const auto& mat : produced )
        {
            // Under the SAME root the migration was measured against: the relative path inside the file
            // and the file on disk must agree about one root, or the source names a material that is not
            // where it says. That root is the SOURCE FILE'S and not the process's working directory —
            // see the header for what the working directory cost.
            const std::filesystem::path matPath = ( assetsRoot / mat.RelativePath ).lexically_normal();

            // TWO FILES MUST NOT LAND ON ONE MATERIAL FILE. The name is derived from the source's own
            // name, which is NOT unique by construction — a .desce copied from another and edited keeps
            // the original's name, and this repository's own verification protocol relies on exactly that
            // copying. Two such files would produce one path here, the second write would take the
            // first's look, and BOTH would then name a file that describes only one of them: a silent
            // whole-sky loss with nothing in the log. The migration function is pure and per-file, so it
            // cannot see the collision; this loop is the only place in the run that can. Named and fatal,
            // never resolved by guessing at a suffix — the fix is to give the file its own name, which is
            // what the operator has to know.
            const auto entry = claimed.emplace( matPath.generic_string(), source.string() );
            if ( !entry.second && entry.first->second != source.string() )
            {
                err << "FAIL   " << source.string() << " — its cloud material would be written to "
                    << matPath.string() << ", which " << entry.first->second
                    << " already claimed in this run: both state the same name. Give one of them its own "
                    << "name and re-run; neither file is modified.\n";
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories( matPath.parent_path(), ec );
            if ( !WriteText( matPath, mat.Json, err ) )
            {
                err << "FAIL   " << matPath.string() << " — the cloud material could not be written; "
                    << source.string() << " is left at its old version\n";
                return false;
            }
            // The FULL path, not the relative name it used to print. An operator reading "wrote
            // Materials/M_X.demat" cannot tell which of two trees it landed in, which is precisely the
            // question this defect turned on; the line now answers it.
            out << "        wrote " << matPath.string() << "\n";
        }
        return true;
    }

    // Writes the `.danimgraph` files the v20 -> v21 step produced, BEFORE the source file that names
    // them, on exactly the terms WriteCloudMaterials states. Returns false on failure, having named it.
    //
    // THE COLLISION RULE IS THE OPPOSITE OF THE MATERIAL ONE, AND THAT IS THE WHOLE POINT OF THE STEP.
    // A cloud material is named after its SCENE, so two scenes landing on one path means one sky is about
    // to be lost and the run must stop. A graph is named after ITSELF, so two entities — in one scene or
    // in twenty — landing on one path is the migration doing its job: a walk graph that was copied into
    // four characters as four identical blobs becomes ONE file that all four share, which is the defect
    // §5.1 named. So a repeated path is fine WHEN THE BYTES AGREE, and fatal when they do not: two
    // different state machines that happen to carry one Name would otherwise silently become whichever of
    // them was written last, with every character on the loser pointing at the winner's graph.
    //
    // The map therefore holds the CONTENT, not the source file name: "who claimed it first" cannot answer
    // "is it the same graph", and this is the one place in the run that can see both.
    bool WriteAnimGraphs( const std::vector<Desert::Migration::AnimGraphFile>& produced,
                          const std::filesystem::path& assetsRoot, const std::filesystem::path& source,
                          std::map<std::string, std::string>& claimed, std::ostream& out, std::ostream& err )
    {
        for ( const auto& graph : produced )
        {
            // Under the SAME root the migration was measured against, for WriteCloudMaterials' reason:
            // the relative path inside the file and the file on disk must agree about one root, and that
            // root is the SOURCE FILE'S rather than the process's working directory.
            const std::filesystem::path graphPath = ( assetsRoot / graph.RelativePath ).lexically_normal();

            const auto entry = claimed.emplace( graphPath.generic_string(), graph.Json );
            if ( !entry.second )
            {
                if ( entry.first->second == graph.Json )
                {
                    // The same graph, reached a second time. Written once, shared from here on — say so,
                    // because "one file, four characters" is the outcome an operator is checking for.
                    out << "        shares " << graphPath.string() << "\n";
                    continue;
                }
                err << "FAIL   " << source.string() << " — its anim graph would be written to "
                    << graphPath.string()
                    << ", which a different graph already claimed in this run: two state machines state "
                    << "the same Name and are not the same graph. Rename one and re-run; no file is "
                    << "modified.\n";
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories( graphPath.parent_path(), ec );
            if ( !WriteText( graphPath, graph.Json, err ) )
            {
                err << "FAIL   " << graphPath.string() << " — the anim graph could not be written; "
                    << source.string() << " is left at its old version\n";
                return false;
            }
            out << "        wrote " << graphPath.string() << "\n";
        }
        return true;
    }

    // The v22 -> v23 step's tile files. Each is named by a tile id derived from its entity's, so a path is
    // never claimed twice in a run; a failed write stops the file, which then keeps its old version and its
    // Terrain block. Encoded by the step - here they are only written.
    bool WriteLandscapeTiles( const std::vector<Desert::Migration::LandscapeTileFile>& produced,
                              const std::filesystem::path& source, std::ostream& out, std::ostream& err )
    {
        for ( const auto& tile : produced )
        {
            const std::string bytes( tile.Bytes.begin(), tile.Bytes.end() );
            std::error_code   ec;
            std::filesystem::create_directories( tile.Path.parent_path(), ec );
            if ( !Common::Utils::FileSystem::WriteContentToFileAtomic( tile.Path, bytes ) )
            {
                err << "FAIL   " << tile.Path.string() << " — the landscape tile could not be written; "
                    << source.string() << " is left at its old version\n";
                return false;
            }
        }
        if ( !produced.empty() )
            out << "        wrote " << produced.size() << " landscape tile file(s) under "
                << produced.front().Path.parent_path().string() << "\n";
        return true;
    }

    // WHAT THE CHAIN DID, in one line, for a scene OR a prefab: the same report comes back from
    // both entry points, so the same function prints it and no step can be reported in one file class
    // and silently omitted in the other. §4.7 - a migration that says nothing is a migration nobody can
    // check.
    //
    // `canonical` is null for a prefab, which has no scene-wide Settings block to canonicalise - the
    // same structural argument the chain itself makes with its settings pointer.
    void PrintSteps( std::ostream& out, const Desert::Migration::FileMigrationReport& report,
                     const Desert::Migration::SettingsCanonicalisationReport* canonical )
    {
        if ( report.SkyRaised )
            out << " sky v0->v" << Desert::Migration::kSceneVersionSky << " (" << report.Sky.Entities
                << " entity(ies), " << report.Sky.FieldsCarried << " carried, " << report.Sky.FieldsRejected
                << " rejected)";
        if ( report.TonemapperRaised )
            out << " scene v" << Desert::Migration::kSceneVersionSky << "->v"
                << Desert::Migration::kSceneVersionTonemap << " ("
                << ( report.Tonemap.OperatorPinned ? "tonemapper pinned to Reinhard"
                                                   : "tonemapper NOT pinned — see the warning above" )
                << ( report.Tonemap.SettingsCreated ? ", settings block created" : "" ) << ")";
        if ( report.CloudNoiseRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionTonemap << "->v"
                << Desert::Migration::kSceneVersionCloudNoise << " (";
            if ( report.CloudNoise.Entities > 0 )
                out << report.CloudNoise.FieldsDropped << " cloud bake setting(s) dropped from "
                    << report.CloudNoise.Entities << " entity(ies)";
            else
                out << "stamp only — no cloud layer carried a bake setting";
            out << ")";
        }
        // The three cloud steps below went unreported until 2026-09-06: Changed() was true, the file
        // was rewritten, and the report line skipped straight from v3 to v6 — a silent migration,
        // which §4.7 forbids ("log which scene, from which version to which, and how many fields
        // moved"). Their counters existed all along; only the printing was missing.
        if ( report.CloudSpeciesRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionCloudNoise << "->v"
                << Desert::Migration::kSceneVersionCloudSpecies << " (";
            if ( report.CloudSpecies.Entities > 0 )
                out << report.CloudSpecies.FieldsDropped << " authored shell field(s) dropped and "
                    << report.CloudSpecies.SpeciesSet << " scalar type(s) turned into a species on "
                    << report.CloudSpecies.Entities << " entity(ies)";
            else
                out << "stamp only — no cloud layer carried the scalar-type shape";
            out << ")";
        }
        if ( report.CloudTypeRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionCloudSpecies << "->v"
                << Desert::Migration::kSceneVersionCloudType << " (";
            if ( report.CloudType.Entities > 0 )
            {
                out << report.CloudType.TypesSet << " species enumerator(s) became a .decloudtype path on "
                    << report.CloudType.Entities << " entity(ies)";
                // Named loud, not folded into the count: these two are the cases the operator has to
                // act on — a layer that lost its noise volume renders a different sky until re-pointed.
                if ( report.CloudType.VolumesLost > 0 )
                    out << "; " << report.CloudType.VolumesLost
                        << " layer noise volume(s) DROPPED — re-point them on the cloud type";
                if ( report.CloudType.FieldsBroken > 0 )
                    out << "; " << report.CloudType.FieldsBroken
                        << " unreadable species value(s) left at the default";
            }
            else
            {
                out << "stamp only — no cloud layer named a species";
            }
            out << ")";
        }
        if ( report.CloudSetRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionCloudType << "->v"
                << Desert::Migration::kSceneVersionCloudSet << " (";
            if ( report.CloudSet.Entities > 0 )
                out << report.CloudSet.SlotsCarried << " cloud type(s) moved into slot 1 of the set on "
                    << report.CloudSet.Entities << " entity(ies), " << report.CloudSet.SlotsEmpty
                    << " of them the empty handle";
            else
                out << "stamp only — no cloud layer carried a single-type key";
            out << ")";
        }
        if ( report.TerrainMaterialRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionCloudSet << "->v"
                << Desert::Migration::kSceneVersionTerrainMaterial << " (";
            if ( report.TerrainMaterial.Entities > 0 )
            {
                // Named, not counted, and for the same reason the loader names them: this step DROPS the
                // values it finds, so the operator running this tool has to be able to see what left.
                out << "inline terrain material removed from " << report.TerrainMaterial.Entities
                    << " entity(ies), dropping " << report.TerrainMaterial.Params << " param(s) and "
                    << report.TerrainMaterial.Textures << " texture(s):";
                for ( const auto& name : report.TerrainMaterial.DroppedNames )
                    out << " " << name;
            }
            else
            {
                out << "stamp only — no terrain entity carried an inline material";
            }
            out << ")";
        }
        if ( report.MaterialPathRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionTerrainMaterial << "->v"
                << Desert::Migration::kSceneVersionMaterialPath << " (";
            if ( report.MaterialPath.Paths > 0 )
                out << report.MaterialPath.Paths << " material path(s) made relative to the assets "
                    << "root in " << report.MaterialPath.Entities << " entity(ies)";
            else
                out << "stamp only - no entity named a material by an absolute path";
            // Named, not counted, for the reason the terrain step names its drops: these are the ones the
            // step could not fix, and the operator has to be able to see which slot to re-point.
            for ( const auto& name : report.MaterialPath.OutsideNames )
                out << "; OUTSIDE the assets root, left absolute: " << name;
            out << ")";
        }
        if ( report.GravityUnitsRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionMaterialPath << "->v"
                << Desert::Migration::kSceneVersionGravityUnits << " (";
            if ( !report.GravityUnits.Found )
                out << "stamp only - the scene states no gravity";
            else if ( report.GravityUnits.Scaled )
                out << "gravity " << report.GravityUnits.Before << " -> " << report.GravityUnits.After
                    << " cm/s2 (metre-era value, x100)";
            else if ( report.GravityUnits.Unrecognised )
                // Named rather than counted, for the same reason the two steps above name what they could
                // not fix: this is the one case the operator has to look at by hand.
                out << "gravity " << report.GravityUnits.Before
                    << " LEFT UNCHANGED - neither Earth in metres nor in centimetres, so it was not "
                       "guessed at";
            else if ( report.GravityUnits.Tidied )
                out << "gravity " << report.GravityUnits.Before << " -> " << report.GravityUnits.After
                    << " cm/s2 (already centimetres; dropped the earlier pass's rounding)";
            else
                out << "gravity already " << report.GravityUnits.After << " cm/s2, unchanged";
            out << ")";
        }
        if ( report.UIVisibilityRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionGravityUnits << "->v"
                << Desert::Migration::kSceneVersionUIVisibility << " (";
            if ( report.UIVisibility.Entities > 0 )
                out << report.UIVisibility.FlagsDropped << " interaction flag(s) folded into Hit Test on "
                    << report.UIVisibility.Entities << " element(s), " << report.UIVisibility.HitTestSet
                    << " of which stopped being the default";
            else
                out << "stamp only - no UI element stated an interaction flag";
            // Named, not counted, for the reason the two steps above name what they could not carry: an
            // element whose flag was unreadable keeps the default, and the operator has to see which one.
            for ( const auto& name : report.UIVisibility.BrokenNames )
                out << "; NOT a boolean, left at the default Hit Test: " << name;
            out << ")";
        }
        if ( report.SSRUnitsRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionUIVisibility << "->v"
                << Desert::Migration::kSceneVersionSSRUnits << " (";
            if ( report.SSRUnits.Scaled )
                out << "SSR max distance " << report.SSRUnits.Before << " -> " << report.SSRUnits.After
                    << " cm (metre-era slider value, x100)";
            else
                out << "stamp only - the scene states no SSR max distance";
            out << ")";
        }
        if ( report.CloudMaterialRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionSSRUnits << "->v"
                << Desert::Migration::kSceneVersionCloudMaterial << " (";
            if ( report.CloudMaterial.Entities > 0 )
            {
                out << report.CloudMaterial.ValuesMoved << " value(s) and " << report.CloudMaterial.AssetsMoved
                    << " asset slot(s) moved into " << report.CloudMaterial.Materials.size()
                    << " bespoke cloud material(s), " << report.CloudMaterial.DefaultsAssigned
                    << " layer(s) pointed at the shared " << Desert::Migration::kDefaultCloudMaterialRelativePath
                    << " (D-37), " << report.CloudMaterial.Defaulted << " field(s) left at the schema default";
            }
            else
                out << "stamp only - no VolumetricCloud payload in this scene";
            // Named, not counted, like every step above that can refuse a value: a rejected number is
            // an authored one that will now read as the default, and the operator has to see which.
            for ( const auto& name : report.CloudMaterial.RejectedNames )
                out << "; NOT carried, schema default stands: " << name;
            out << ")";
        }
        if ( report.AnimGraphRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionTextKeySigil << "->v"
                << Desert::Migration::kSceneVersionAnimGraphAsset << " (";
            if ( report.AnimGraph.Entities > 0 || report.AnimGraph.Empty > 0 )
            {
                out << report.AnimGraph.Entities << " state machine(s) moved into "
                    << report.AnimGraph.Graphs.size() << " file(s), " << report.AnimGraph.Empty
                    << " empty GraphJson key(s) dropped";
            }
            else
                out << "stamp only - no Animation payload in this file states a graph";
            // Named, not counted, like every step above that can refuse a value: a rejected blob is an
            // authored state machine that did NOT move, and it is still in the file.
            for ( const auto& name : report.AnimGraph.RejectedNames )
                out << "; LEFT IN PLACE, not moved: " << name;
            out << ")";
        }
        if ( report.EditMeshRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionAnimGraphAsset << "->v"
                << Desert::Migration::kSceneVersionEditMesh << " (";
            if ( report.EditMesh.Entities > 0 )
                out << report.EditMesh.Entities << " edited mesh(es) now saved as an EditMesh:";
            else
                out << "stamp only - no StaticMesh payload in this file states CustomVertices";
            // Named, not counted: the conversion is a WELD by distance, and the operator has to be able to
            // see what it made of each mesh; a rejected one is still in the file, under its v21 keys.
            for ( const auto& name : report.EditMesh.ConvertedNames )
                out << " " << name << ";";
            for ( const auto& name : report.EditMesh.RejectedNames )
                out << "; LEFT IN PLACE, not converted: " << name;
            out << ")";
        }
        if ( report.ProceduralTerrainRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionEditMesh << "->v"
                << Desert::Migration::kSceneVersionProceduralTerrain << " (";
            if ( report.ProceduralTerrain.Entities > 0 )
                out << report.ProceduralTerrain.Entities << " procedural terrain(s) baked into "
                    << report.ProceduralTerrain.Tiles << " landscape tile(s):";
            else
                out << "stamp only - no Terrain payload in this file";
            for ( const auto& name : report.ProceduralTerrain.ConvertedNames )
                out << " " << name << ";";
            for ( const auto& name : report.ProceduralTerrain.RejectedNames )
                out << "; LEFT IN PLACE, not baked: " << name;
            out << ")";
        }
        if ( report.TextureAssetRefsRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionProceduralTerrain << "->v"
                << Desert::Migration::kSceneVersionTextureAssetRefs << " (";
            if ( report.TextureAssetRefs.Rewritten > 0 )
                out << report.TextureAssetRefs.Rewritten << " texture reference(s) now name the asset:";
            else
                out << "stamp only - no cooked:Textures/ reference in this file";
            for ( const auto& name : report.TextureAssetRefs.RewrittenNames )
                out << " " << name << ";";
            for ( const auto& name : report.TextureAssetRefs.MissingNames )
                out << "; NO ASSET under the assets root: " << name;
            out << ")";
        }
        if ( report.SiblingOrderRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionTextureAssetRefs << "->v"
                << Desert::Migration::kSceneVersionSiblingOrder << " (" << report.SiblingOrder.Indexed
                << " sibling index(es) stated, " << report.SiblingOrder.Reordered
                << " record(s) moved when sorted by id)";
        }
        if ( report.DebugViewRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionCloudMaterial << "->v"
                << Desert::Migration::kSceneVersionDebugView << " (";
            if ( report.DebugView.KeysRemoved > 0 )
            {
                // Named with their values, not counted: these were AUTHORED flags, and the operator has
                // to see that (say) the collider wireframes stopped because the file stopped deciding
                // them - not because something broke.
                out << report.DebugView.KeysRemoved << " viewport debug key(s) removed - the view owns "
                    << "them now (editor Show flags / View Mode):";
                for ( const auto& name : report.DebugView.RemovedNames )
                    out << " " << name;
            }
            else
            {
                out << "stamp only - the scene stated no viewport debug flag";
            }
            out << ")";
        }
        if ( report.ScriptRootRaised )
        {
            // v15, not the previous PRINTED step (v13): 14 and 15 are rows of kRetiredKeys rather
            // than steps of their own, so the last line above this one names 13 and a file arriving
            // here is at 15. Printing the previous printed number would report a transition no file
            // made — the same wrong-transition trap the retired-keys line below documents.
            out << " scene v" << Desert::Migration::kSceneVersionMachineQuality << "->v"
                << Desert::Migration::kSceneVersionScriptRoot << " (";
            if ( report.ScriptRoot.Slots > 0 )
                out << report.ScriptRoot.Slots << " script reference(s) root-tagged on "
                    << report.ScriptRoot.Entities << " entity(ies), " << report.ScriptRoot.Empty
                    << " of them an empty slot";
            else if ( report.ScriptRoot.UnrootedNames.empty() )
                out << "stamp only - no entity named a script";
            else
                out << "no reference could be root-tagged";
            // Named, not counted, like every step above that can refuse a value: a reference the
            // census could not place still does not resolve in a packaged game, and the operator has
            // to see which entity to re-point.
            for ( const auto& name : report.ScriptRoot.UnrootedNames )
                out << "; NOT under a Scripts/ folder, carried over untagged: " << name;
            out << ")";
        }
        if ( report.ServiceAssetRootRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionScriptRoot << "->v"
                << Desert::Migration::kSceneVersionServiceAssetRoot << " (";
            if ( report.ServiceAssetRoot.Refs > 0 )
                out << report.ServiceAssetRoot.Refs << " font/icon/video reference(s) root-tagged on "
                    << report.ServiceAssetRoot.Entities << " entity(ies), " << report.ServiceAssetRoot.Empty
                    << " of them an empty slot";
            else if ( report.ServiceAssetRoot.UnrootedNames.empty() )
                out << "stamp only - no entity named a font, an icon or a video";
            else
                out << "no reference could be root-tagged";
            // Named, not counted, like every step above that can refuse a value: a reference neither
            // root can place still does not resolve in a packaged game.
            for ( const auto& name : report.ServiceAssetRoot.UnrootedNames )
                out << "; under NEITHER content root, carried over untagged: " << name;
            out << ")";
        }
        if ( report.GrassGenerationRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionServiceAssetRoot << "->v"
                << Desert::Migration::kSceneVersionGrassGeneration << " (";
            if ( report.GrassGeneration.Entities == 0 )
                out << "stamp only - no terrain stated a grass generator key";
            else
                out << report.GrassGeneration.KeysRemoved << " grass generator key(s) removed from "
                    << report.GrassGeneration.Entities << " terrain(s)";
            // Named with their values, because this is the only place the numbers a designer authored
            // for the generator are ever said again.
            for ( const auto& name : report.GrassGeneration.RemovedNames )
                out << " " << name;
            // And the carry, separately: it is the half of the step that changes what is DRAWN.
            for ( const auto& name : report.GrassGeneration.CarriedNames )
                out << "; carried " << name;
            out << ")";
        }
        if ( report.RetiredKeysRaised )
        {
            // NOT a step's own pair of numbers, unlike every line above: the retirement pass is
            // gated on the head and sweeps a file from WHEREVER it was to wherever the head is now
            // (see MigrateRetiredKeys). Printing a fixed 13->14 here would have reported the wrong
            // transition for every file K3 converted, which stood at 14.
            out << " retired keys -> v" << Desert::Migration::kSceneVersion << " (";
            if ( report.RetiredKeys.KeysRemoved > 0 )
            {
                // Named with their values AND the reason, because from v14 on this is the ONLY way a
                // key ever leaves a file: the saver preserves everything it does not declare, so a
                // removal is always a decision somebody made and the operator is entitled to see it.
                out << report.RetiredKeys.KeysRemoved << " retired key(s) removed:";
                for ( const auto& name : report.RetiredKeys.RemovedNames )
                    out << " " << name;
            }
            else
            {
                out << "stamp only - the scene stated no retired key";
            }
            // A prefab has no Settings block, so there is nothing to canonicalise and no clause to
            // print — not an omission from the line, an absence in the file.
            if ( canonical != nullptr )
            {
                if ( canonical->Refused )
                {
                    out << "; Settings NOT canonicalised - see the error above";
                }
                else
                {
                    out << "; Settings canonical (";
                    if ( canonical->BlockCreated )
                        out << "block created, ";
                    out << canonical->KeysAdded << " field(s) the file did not state, "
                        << canonical->ValuesRestated << " restated at float precision)";
                }
            }
            out << ")";
        }
        if ( report.UnitsRaised )
            out << " units v0->v" << Desert::Migration::kUnitVersion << " (" << report.Units.Entities
                << " entity(ies), " << report.Units.Values << " value(s) x100, " << report.Units.Rejected
                << " rejected)";
    }

} // namespace

namespace Desert::Migration
{
    namespace
    {
        // The one sentence both output-root functions are: derive the assets root from the FILE, through
        // the census row for the folder its own class lives in, and never from where the process stands.
        std::filesystem::path OutputRootFor( Common::Constants::Path::ContentDir dir,
                                             const std::filesystem::path&        contentPath )
        {
            if ( const auto root = Common::Constants::Path::RootForContentPath( dir, contentPath ) )
                return *root;

            // Not under the census folder, so it has nothing to say: the file's own directory is the
            // root. `parent_path()` is empty for a bare `x.desce`, and an empty root would resolve the
            // material against the working directory by a different route — the very thing being fixed —
            // so it is spelled as the current directory explicitly.
            const std::filesystem::path directory = contentPath.parent_path();
            return directory.empty() ? std::filesystem::path( "." ) : directory;
        }
    } // namespace

    std::filesystem::path SceneOutputRoot( const std::filesystem::path& scenePath )
    {
        return OutputRootFor( Common::Constants::Path::ContentDir::Scene, scenePath );
    }

    std::filesystem::path PrefabOutputRoot( const std::filesystem::path& prefabPath )
    {
        return OutputRootFor( Common::Constants::Path::ContentDir::Prefab, prefabPath );
    }

    int RunSceneMigrator( const std::vector<std::string>& args, std::ostream& out, std::ostream& err )
    {
        bool                               check = false;
        std::vector<std::filesystem::path> roots;

        for ( const std::string& arg : args )
        {
            if ( arg == "--check" )
                check = true;
            else
                roots.emplace_back( arg );
        }

        if ( roots.empty() )
        {
            err << "usage: SceneMigrator [--check] <scene.desce | material.demat | prefab.deprefab | "
                   "clip.anim | directory>...\n";
            return 2;
        }

        std::vector<std::filesystem::path> scenes;
        std::vector<std::filesystem::path> materials;
        std::vector<std::filesystem::path> prefabs;
        std::vector<std::filesystem::path> clips;
        for ( const auto& root : roots )
            Collect( root, scenes, materials, prefabs, clips );

        if ( scenes.empty() && materials.empty() && prefabs.empty() && clips.empty() )
        {
            err << "SceneMigrator: no " << kSceneExtension << ", " << kMaterialExtension << ", "
                << kPrefabExtension << " or " << kClipExtension << " files found\n";
            return 2;
        }

        int changed = 0;
        int failed  = 0;
        int relaid  = 0;

        // Cloud material FILE -> the scene that produced it, for the collision check below. Keyed on the
        // resolved path and not on the relative name any more: the root is per-scene now, so two scenes
        // sharing a SceneName under two different assets roots produce two different files and are not in
        // conflict at all, while the case the guard exists for — one file, two scenes — is exactly a
        // repeated key here.
        std::map<std::string, std::string> writtenMaterials;

        // The same guard for the graphs, and it holds CONTENT rather than the claiming file — see
        // WriteAnimGraphs for why a repeated path is the intended outcome there and a fatal one here.
        // Shared by the scene pass and the prefab pass, for the reason `writtenMaterials` is: two files of
        // different classes minting one graph name have to be seen through ONE map.
        std::map<std::string, std::string> writtenGraphs;

        for ( const auto& path : scenes )
        {
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }

            auto parsed = rfl::json::read<Desert::Migration::SceneSerialized>( source );
            if ( !parsed )
            {
                err << "FAIL   " << path.string() << " — " << parsed.error().what() << "\n";
                ++failed;
                continue;
            }

            // ONE root for this scene, and it is the scene's own (see SceneOutputRoot). It is handed to
            // the migration as well as used for the write below, so the root the v7 -> v8 step measures
            // material paths against and the root the v11 -> v12 material is written under are the same
            // root by construction — the two used to be one global read twice, which is how a path
            // written into the scene could name a place the file was not.
            const std::filesystem::path assetsRoot = SceneOutputRoot( path );

            const Desert::Migration::FileMigrationReport report =
                 Desert::Migration::MigrateScene( parsed.value(), assetsRoot, path );

            // A scene from a LATER build: nothing ran and nothing was stamped, so this is a FAILED file
            // and not an "ok". It used to be neither — the tree fell through every gate and was stamped
            // DOWN to this tool's head, then reported as already current.
            if ( !report.Refused.empty() )
            {
                err << "FAIL   " << path.string() << " — " << report.Refused << "\n";
                ++failed;
                continue;
            }

            // CANONICALISATION IS THE TOOL'S, NOT A SCHEMA STEP'S, and the split is structural rather
            // than tidiness. Every function in SceneMigration.hpp is pure over the parsed tree, which is
            // what lets sixteen suites compile that one translation unit and test a step each with no
            // engine linked; this needs the engine's reflection table, so it lives beside main where the
            // table is already paid for. It runs on the same gate as the retirement step and AFTER it —
            // canonical means "the fields the table describes", and a retired key is by definition not
            // one of them, so running it first would drop the key with nothing left to report.
            const Desert::Migration::SettingsCanonicalisationReport canonical =
                 report.RetiredKeysRaised ? Desert::Migration::CanonicaliseSettings( parsed.value().Settings )
                                          : Desert::Migration::SettingsCanonicalisationReport{};

            if ( !report.Changed() )
            {
                if ( const Layout layout = RelayOutIfNeeded( path, source, check, out, err );
                     layout != Layout::Canonical )
                {
                    ++( layout == Layout::Failed ? failed : relaid );
                    continue;
                }
                out << "ok     " << path.string() << " — already at scene v" << Desert::Migration::kSceneVersion
                    << " / units v" << Desert::Migration::kUnitVersion << "\n";
                continue;
            }

            out << ( check ? "WOULD  " : "raised " ) << path.string() << " —";
            PrintSteps( out, report, &canonical );
            out << "\n";

            if ( check )
            {
                ++changed;
                continue;
            }

            if ( !WriteCloudMaterials( report.CloudMaterial.Materials, assetsRoot, path, writtenMaterials, out,
                                       err ) )
            {
                ++failed;
                continue;
            }

            if ( !WriteAnimGraphs( report.AnimGraph.Graphs, assetsRoot, path, writtenGraphs, out, err ) )
            {
                ++failed;
                continue;
            }

            if ( !WriteLandscapeTiles( report.ProceduralTerrain.Files, path, out, err ) )
            {
                ++failed;
                continue;
            }

            // Write-then-rename, and the verdict comes from the WRITE rather than from the open. This
            // used to open the scene ITSELF with trunc and check only that the open worked — so by the
            // time a full disk, dropped permissions or a killed process stopped the write, the scene
            // was already zero bytes, and the tool still printed "raised" and exited 0 over the wreck.
            // The atomic primitive never opens the original at all; a failure at any step leaves it
            // byte-identical, and is a failed FILE here: counted, named, fatal to the exit code like
            // every FAIL above. The "raised" line above then describes work that was NOT kept, which is
            // why this line says so explicitly.
            if ( !WriteText( path, rfl::json::write( parsed.value() ), err ) )
            {
                err << "FAIL   " << path.string() << " — the raise could not be written; the original file "
                    << "is untouched\n";
                ++failed;
                continue;
            }
            ++changed;
        }

        // THE MATERIALS, AFTER the scenes — a scene's v11 -> v12 raise WRITES `.demat` files, and those
        // are already produced with the current slot names (MigrateCloudMaterialV11ToV12 calls the same
        // step), so this pass finds nothing to do in them and says so. Running it first would depend on
        // whether the file existed yet, which is an ordering nobody should have to know about.
        int materialsChanged = 0;
        int clipsChanged     = 0;
        for ( const auto& path : materials )
        {
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }

            auto parsed = rfl::json::read<Desert::Assets::MaterialData>( source );
            if ( !parsed )
            {
                err << "FAIL   " << path.string() << " — " << parsed.error().what() << "\n";
                ++failed;
                continue;
            }

            const Desert::Migration::CloudMaterialLayoutReport report =
                 Desert::Migration::MigrateCloudMaterialLayoutInputs( parsed.value() );
            // BOTH STEPS ALWAYS RUN, and neither short-circuits the other: a `.demat` can need the albedo
            // raise without ever having had a `CloudLayout` binding, and returning early on the first
            // report is how a file gets certified "ok" while still carrying a scalar albedo — which
            // renders as a RED sky, not as a missing feature.
            const Desert::Migration::CloudMaterialAlbedoReport albedo =
                 Desert::Migration::MigrateCloudMaterialAlbedoToColour( parsed.value() );

            if ( !report.Changed() && !albedo.Changed() )
            {
                if ( const Layout layout = RelayOutIfNeeded( path, source, check, out, err );
                     layout != Layout::Canonical )
                {
                    ++( layout == Layout::Failed ? failed : relaid );
                    continue;
                }
                out << "ok     " << path.string() << " — no pre-O-4 layout slot, no scalar albedo\n";
                continue;
            }

            // WHAT WAS RAISED, phrased ONCE and printed only where it is true. It used to be printed
            // before the write, which meant a refused write reported "raised <file>" and then "FAIL
            // <file>" — a claim of an action that had not happened, beside the correction. That is the
            // Д31 class with its sign flipped, and it was found by making the write refuse on purpose.
            std::ostringstream what;
            if ( report.Changed() )
                what << " " << report.Split
                     << " CloudLayout binding(s) split into LayoutPattern + LayoutMask, both naming the "
                        "same painting;";
            if ( albedo.Changed() )
                what << " " << albedo.Broadcast
                     << " scalar ScatteringAlbedo value(s) broadcast to a neutral colour;";

            if ( check )
            {
                out << "WOULD  " << path.string() << " —" << what.str() << "\n";
                ++materialsChanged;
                continue;
            }

            if ( !WriteText( path, rfl::json::write( parsed.value() ), err ) )
            {
                err << "FAIL   " << path.string() << " — the raise could not be written; the original file "
                    << "is untouched. It would have been:" << what.str() << "\n";
                ++failed;
                continue;
            }
            out << "raised " << path.string() << " —" << what.str() << "\n";
            ++materialsChanged;
        }

        // ---- THE CLIPS ----------------------------------------------------------------------------
        //
        // Their own generation sequence, their own gate. A `.anim` with no version field is generation 0
        // and is converted; one already at the current generation is REFUSED by the migration function
        // rather than converted again — this step is NOT idempotent, and reading integer ticks as seconds
        // is exactly the doubling the text-sigil step once shipped (`#menu.play` -> `##menu.play`).
        for ( const auto& path : clips )
        {
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }

            Desert::Assets::Serialization::AnimationMigrationReport report;
            const auto migrated = Desert::Assets::Serialization::MigrateAnimationJson( source, report );
            if ( !migrated )
            {
                // A clip already at the current generation is the ordinary case on a second run, and it is
                // reported as "ok" rather than as a failure — the refusal is what keeps a second run from
                // doubling the conversion, not a sign that anything is wrong.
                if ( report.FromVersion >= Desert::Assets::Serialization::kAnimationVersion )
                {
                    if ( const Layout layout = RelayOutIfNeeded( path, source, check, out, err );
                         layout != Layout::Canonical )
                    {
                        ++( layout == Layout::Failed ? failed : relaid );
                        continue;
                    }
                    out << "ok     " << path.string() << " — already at `.anim` generation " << report.FromVersion
                        << "\n";
                    continue;
                }
                err << "FAIL   " << path.string() << " — " << migrated.GetError() << "\n";
                ++failed;
                continue;
            }

            // WHAT ACTUALLY HAPPENED TO THIS FILE, which is not the same sentence for both steps. A
            // generation-0 file has its key times moved from float seconds onto the tick grid; a
            // generation-1 one keeps every tick it had and gains the SHAPE each key now states. Printing
            // the first sentence for both was true of the corpus on the day it was written and false the
            // day a second step existed.
            std::ostringstream what;
            if ( report.FromVersion < 1 )
            {
                what << " generation " << report.FromVersion << ": key times moved from float seconds onto "
                     << "the " << Desert::Animation::PROJECT_TICK_RATE.Numerator << "-tick grid; display rate "
                     << report.DisplayRateNumerator << " fps"
                     << ( report.DisplayRateIsAFallback ? " (no standard grid fits its keys, defaulted)" : "" )
                     << "; " << report.KeysMoved << " key time(s) rounded";
                if ( report.KeysMoved > 0 )
                {
                    what << ", worst by " << report.WorstMicro << " millionths of a tick";
                }
                what << ";";
            }
            else
            {
                what << " generation " << report.FromVersion << ": every tick kept; display rate "
                     << report.DisplayRateNumerator << " fps carried;";
            }
            what << " " << report.ShapesWritten
                 << " key(s) now STATE their interpolation and tangent mode instead of inheriting a silent "
                    "default;";
            // THE GENERATION-3 SENTENCE, and it is a separate one because it is a separate claim: the
            // step from 2 adds NO key shapes (they were already stated) and adds the section instead, so
            // a line reporting only `ShapesWritten` would say "0" and read as a conversion that did
            // nothing.
            what << " " << report.SectionsWritten
                 << " section(s) now STATE the range, blend type and weight its values are read under;";

            if ( check )
            {
                out << "WOULD  " << path.string() << " —" << what.str() << "\n";
                ++clipsChanged;
                continue;
            }

            if ( !WriteText( path, migrated.GetValue(), err ) )
            {
                err << "FAIL   " << path.string() << " — the conversion could not be written; the original "
                    << "file is untouched. It would have been:" << what.str() << "\n";
                ++failed;
                continue;
            }
            out << "raised " << path.string() << " —" << what.str() << "\n";
            ++clipsChanged;
        }

        // THE PREFABS, through the SAME chain the scenes went through (И11). They come last for the
        // reason the materials do: a prefab's v11 -> v12 raise can produce a `.demat` under a name a
        // scene may already have claimed in this run, and `writtenMaterials` is the one map that sees it.
        int prefabsChanged = 0;
        for ( const auto& path : prefabs )
        {
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }

            // rfl::json::read and NOT the engine's ParseLoadablePrefab: that gate REFUSES everything but
            // the head, which is precisely the population this tool exists to convert. The gate is
            // applied to the OUTPUT instead, below, where it belongs.
            auto parsed = rfl::json::read<Desert::Assets::PrefabData>( source );
            if ( !parsed )
            {
                err << "FAIL   " << path.string() << " — " << parsed.error().what() << "\n";
                ++failed;
                continue;
            }

            const std::filesystem::path assetsRoot = PrefabOutputRoot( path );

            const Desert::Migration::PrefabMigrationOutcome outcome =
                 Desert::Migration::MigratePrefab( parsed.value(), assetsRoot, path );

            if ( !outcome.Refused.empty() )
            {
                err << "FAIL   " << path.string() << " — " << outcome.Refused << "\n";
                ++failed;
                continue;
            }
            if ( outcome.AlreadyCurrent )
            {
                if ( const Layout layout = RelayOutIfNeeded( path, source, check, out, err );
                     layout != Layout::Canonical )
                {
                    ++( layout == Layout::Failed ? failed : relaid );
                    continue;
                }
                out << "ok     " << path.string() << " — already at scene v" << Desert::Migration::kSceneVersion
                    << " / units v" << Desert::Migration::kUnitVersion << "\n";
                continue;
            }

            out << ( check ? "WOULD  " : "raised " ) << path.string() << " —";
            if ( outcome.StampOnly )
            {
                // The one case with no step behind it, and it says so rather than printing an empty
                // line: an operator whose prefab looks wrong afterwards has to be able to see that this
                // file was stamped on an ASSUMPTION about its generation, not migrated.
                out << " unversioned (v0/v0) stamped to scene v" << Desert::Migration::kSceneVersion
                    << " / units v" << Desert::Migration::kUnitVersion
                    << " (stamp only; entities untouched — an unstamped prefab states no generation to "
                       "migrate FROM)";
            }
            else
            {
                out << " from scene v" << outcome.FoundSceneVersion << ":";
                // No canonicalisation clause: a prefab has no Settings block (see PrintSteps).
                PrintSteps( out, outcome.Steps, nullptr );
            }
            out << "\n";

            if ( check )
            {
                ++prefabsChanged;
                continue;
            }

            if ( !WriteCloudMaterials( outcome.Steps.CloudMaterial.Materials, assetsRoot, path, writtenMaterials,
                                       out, err ) )
            {
                ++failed;
                continue;
            }

            if ( !WriteAnimGraphs( outcome.Steps.AnimGraph.Graphs, assetsRoot, path, writtenGraphs, out, err ) )
            {
                ++failed;
                continue;
            }

            if ( !WriteLandscapeTiles( outcome.Steps.ProceduralTerrain.Files, path, out, err ) )
            {
                ++failed;
                continue;
            }

            // Serialized through the ENGINE'S OWN writer, so the bytes written are the bytes the saver
            // would produce, and then gate-checked with the ENGINE'S OWN loader gate before a byte
            // reaches the target: "the tool wrote it" and "the engine will load it" cannot drift. The
            // write itself is the shared atomic primitive — the original is byte-identical on any
            // failure, which is the guarantee И2 had to add after this tool truncated a file it then
            // reported as raised.
            const auto written = Desert::Assets::WritePrefabJson( parsed.value() );
            if ( !written )
            {
                err << "FAIL   " << path.string() << " — " << written.GetError() << " (original untouched)\n";
                ++failed;
                continue;
            }
            const std::string& migrated = written.GetValue();
            if ( auto loadable = Desert::Assets::ParseLoadablePrefab( path.string(), migrated ); !loadable )
            {
                err << "FAIL   " << path.string() << " — the migrated text does not pass the engine's own "
                    << "gate: " << loadable.GetError() << " (original untouched)\n";
                ++failed;
                continue;
            }
            if ( !WriteText( path, migrated, err ) )
            {
                err << "FAIL   " << path.string() << " — the raise could not be written; the original file "
                    << "is untouched\n";
                ++failed;
                continue;
            }
            ++prefabsChanged;
        }

        out << "SceneMigrator: " << scenes.size() << " scene(s), " << changed
            << ( check ? " would change, " : " raised, " ) << clips.size() << " clip(s) of which " << clipsChanged
            << ( check ? " would change, " : " raised, " ) << materials.size() << " material(s), "
            << materialsChanged << ( check ? " would change, " : " raised, " ) << prefabs.size() << " prefab(s), "
            << prefabsChanged << ( check ? " would change, " : " raised, " ) << relaid
            << ( check ? " would be re-laid-out, " : " re-laid-out, " ) << failed << " failed\n";

        if ( failed > 0 )
            return 1;
        return ( check && ( changed > 0 || materialsChanged > 0 || prefabsChanged > 0 || relaid > 0 ) ) ? 1 : 0;
    }
} // namespace Desert::Migration
