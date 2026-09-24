// EVERY ASSET REFERENCE A SHIPPED `.demat` MAKES MUST NAME A FILE THIS REPOSITORY CONTAINS.
//
// A material names its assets by NUMBER and by nothing else: `{"Name":"u_AlbedoTexture",
// "TextureHandle":4588246833979984450}`. There is no path field to fall back on and no filename anywhere
// in the record, so a reference that stops resolving produces a surface that is merely untextured. Nobody
// gets an error naming a file, because no file is named.
//
// WHAT THE NUMBER IS. For most kinds, `Common::AssetHandle::FromCookedPath( <the asset's file> )` — FNV-1a
// over the file's place inside the project behind its root's tag (`assets:CloudTypes/X.decloudtype`). A
// texture is the exception since AF3: a `.detex` carries its handle in its header (Guid.Hi), frozen at
// import to the number its SOURCE image's path derived (`assets:Textures/T_Checker.png`), and the engine
// reads it through ReadTextureAssetKey — the same function TextureAsset::Load uses. So a `.detex` is known
// by its header, never by its own path. Either way one owner states the number, so a material's reference
// and the file it means are two statements of one quantity, and this suite asserts the agreement rather
// than either side. It is the shape the taxonomy in the `desert-engine-verify` skill keeps naming: both
// sides individually plausible, the defect living only in the disagreement.
//
// WHY A CENSUS AND NOT A LIST. Four references in this repository were stale — written before the
// derivation became project-relative — and the way that was found was somebody counting by hand. They
// resolved on the machine that authored them, because nothing re-cooks a texture whose mesh is already
// cooked; on any machine that cooked from scratch the importer minted the derived id and the two `.demat`
// naming the old numbers pointed at nothing. A hand-kept list of known-good numbers would have to be
// edited by the same person who adds the next broken one.
//
// WHY IT LOOKS UNDER `Resources/Assets/` AND NOT UNDER `Cooked/`. Because a cooked file is not evidence:
// `Cooked/` is gitignored, so a fresh checkout and CI have almost none of it, and a machine that HAS one
// can have a stale one — which is precisely the state that hid this defect. Every handle a material can
// legitimately name is stated by a SOURCE file: a texture's by its `.detex` header, a cloud type's by its
// `.decloudtype`, a layout's from its `.dclayout`. Deriving from the sources is what makes this suite mean
// the same thing in CI as on a developer's machine.
//
// WHAT WAS BELIEVED HERE BEFORE, AND WAS WRONG. `AssetHandleStability` states that "no file in the
// repository refers to a path-derived handle BY NUMBER", and that the only persisted references are the
// five texture ones whose ids come out of a `.tex` instead. The census below finds 22 distinct non-zero
// references, and 17 of them ARE path-derived: nine `.decloudtype`, seven `.dclayout` and one `.hdr`,
// reached through material slots named `CloudType1..4`, `CloudLayout` and `samplerCubeMap`. They were
// missed because they were counted with a texture resolver, and they are not textures. So the protection
// that comment describes does not exist, and this suite is it: change the derivation and 17 shipped
// materials go red here instead of silently emptying.

#include <gtest/gtest.h>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>

#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>

// Same serialization environment as SurfaceMaterialAsset.cpp: the glm/UUID adapters plus the json backend.
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Assets::MaterialData;

namespace
{
    namespace fs = std::filesystem;

    // Walks up from the working directory looking for a file only the repository has. Copied in shape from
    // Desert/Tests/Engine/MaterialIdentity, which needs the same thing for the same reason: the test
    // runner's working directory is not fixed. (The suites share no header; copy-paste is the convention
    // this directory already follows.)
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Saves and restores the process-wide project root, so a test that has to point the derivation at this
    // checkout cannot leak that into the next one. Copied in shape from AssetHandleStability's guard.
    class ProjectRootGuard
    {
    public:
        ProjectRootGuard() : m_Saved( Common::Constants::Path::CurrentProjectRoot() )
        {
        }

        ~ProjectRootGuard()
        {
            Common::Constants::Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
        }

        ProjectRootGuard( const ProjectRootGuard& )            = delete;
        ProjectRootGuard& operator=( const ProjectRootGuard& ) = delete;

    private:
        Common::Constants::Path::ProjectRootState m_Saved;
    };

    // ── The three pure pieces the tests are assembled from ─────────────────────────────────────────────
    //
    // They are separate functions, and not one test body, for one reason: the last test in this file has
    // to prove the census can go RED, and the only honest way to do that is to run the SAME code over an
    // input that is broken on purpose. A checker that cannot be handed a bad input has never been shown to
    // reject one.

    // One asset reference, with everything a failure message needs to be actionable.
    struct AssetReference
    {
        std::string File; // the `.demat`, as a repository-relative name
        std::string Slot; // the schema parameter it fills
        uint64_t    Handle = 0;
    };

    // Every non-zero reference every `.demat` under `materialsRoot` makes. A zero handle is an authored
    // "no asset" and is not a reference.
    std::vector<AssetReference> ReferencesUnder( const fs::path& materialsRoot, std::string* parseError )
    {
        std::vector<AssetReference> out;
        for ( const auto& entry : fs::recursive_directory_iterator( materialsRoot ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".demat" )
                continue;

            const auto parsed = rfl::json::read<MaterialData>( ReadAll( entry.path() ) );
            if ( !parsed )
            {
                if ( parseError && parseError->empty() )
                    *parseError = entry.path().string() + ": " + parsed.error().what();
                continue;
            }

            const std::string name = fs::relative( entry.path(), materialsRoot ).generic_string();
            for ( const auto& texture : parsed.value().Textures )
            {
                if ( texture.TextureHandle == 0 )
                    continue;
                out.push_back( { name, texture.Name, texture.TextureHandle } );
            }
        }
        return out;
    }

    // The handle a content file is found by: a texture asset's is the one frozen into its header (read by
    // the engine's own ReadTextureAssetKey, as TextureAsset::Load reads it), every other file's is its path
    // through AssetHandle::FromCookedPath. Not a re-spelling of either rule here: a test that hashes its own
    // idea of `assets:<rel>` or parses its own idea of the header would agree with itself forever while the
    // engine moved.
    uint64_t HandleOfContentFile( const fs::path& file )
    {
        if ( Desert::Assets::IsTextureSourceAssetFile( file ) )
        {
            const auto key = Desert::Assets::ReadTextureAssetKey( file );
            EXPECT_TRUE( key.IsSuccess() ) << key.GetError();
            if ( key.IsSuccess() )
                return static_cast<uint64_t>( key.GetValue().Handle );
        }
        return static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( file ) );
    }

    // handle -> the file that states it, for every file under `contentRoot`.
    std::map<uint64_t, std::string> DerivedHandlesUnder( const fs::path& contentRoot )
    {
        std::map<uint64_t, std::string> out;
        for ( const auto& entry : fs::recursive_directory_iterator( contentRoot ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            out.emplace( HandleOfContentFile( entry.path() ),
                         fs::relative( entry.path(), contentRoot ).generic_string() );
        }
        return out;
    }

    // The relation itself: which references name nothing.
    std::vector<AssetReference> Unresolved( const std::vector<AssetReference>&     references,
                                            const std::map<uint64_t, std::string>& derived )
    {
        std::vector<AssetReference> out;
        for ( const auto& reference : references )
        {
            if ( derived.find( reference.Handle ) == derived.end() )
                out.push_back( reference );
        }
        return out;
    }

    std::string Describe( const AssetReference& reference )
    {
        return "  " + reference.File + "  slot '" + reference.Slot + "'  handle " +
               std::to_string( reference.Handle );
    }
} // namespace

// ── The census ─────────────────────────────────────────────────────────────────────────────────────

TEST( AssetReferenceCensus, EveryReferenceAShippedMaterialMakesNamesAFileInTheProject )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const fs::path materials = root + "Editor/Resources/Assets/Materials";
    const fs::path content   = root + "Editor/Resources/Assets";
    ASSERT_TRUE( fs::exists( materials ) ) << materials.string() << " is missing";

    // The derivation reads the project root out of Constants::Path, so it has to be pointed at THIS
    // checkout — otherwise every path falls outside every root and hashes its absolute spelling, which is
    // the machine-dependent identity the whole scheme exists to avoid.
    ProjectRootGuard guard;
    Common::Constants::Path::SetProjectRoot( root + "Editor", "Resources/Assets" );

    std::string parseError;
    const auto  references = ReferencesUnder( materials, &parseError );
    EXPECT_TRUE( parseError.empty() ) << "a shipped material does not parse: " << parseError;

    // A sweep that found nothing passes vacuously, and the two ways that happens — a wrong root, and a
    // rename of the materials directory — are both silent. The floor is asserted rather than assumed.
    ASSERT_GT( references.size(), 20u )
         << "only " << references.size()
         << " asset references were found across the shipped materials. The sweep is not looking where the "
            "materials are, so it is asserting nothing.";

    const auto derived    = DerivedHandlesUnder( content );
    const auto unresolved = Unresolved( references, derived );

    std::string report;
    for ( const auto& reference : unresolved )
        report += "\n" + Describe( reference );

    EXPECT_TRUE( unresolved.empty() )
         << unresolved.size() << " of " << references.size()
         << " asset references in the shipped materials name no file in the project:" << report
         << "\n\nA material names its assets by number alone — there is no path in the record — so each of "
            "these draws as an unassigned slot with no filename anywhere in the log. The number is "
            "the handle in a `.detex` header for a texture and AssetHandle::FromCookedPath of the file "
            "for every other kind, so a reference stops resolving when a path-keyed asset is renamed, "
            "moved, or deleted, or when a texture is re-imported under a new handle. Fix the material to "
            "name the file's number; do not add the missing file's old id back.";
}

// ── The census must be able to fail ────────────────────────────────────────────────────────────────

// The test above is green, and a green test proves nothing about its own ability to go red. This runs the
// SAME three functions over a materials tree built to be broken, so the red path is exercised on every
// run rather than the day somebody breaks a material.
TEST( AssetReferenceCensus, TheCensusReportsAReferenceThatNamesNothing )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const fs::path content = root + "Editor/Resources/Assets";
    const fs::path scratch = fs::temp_directory_path() / "desert_materialassetreferences_dangling";
    fs::remove_all( scratch );
    fs::create_directories( scratch );

    ProjectRootGuard guard;
    Common::Constants::Path::SetProjectRoot( root + "Editor", "Resources/Assets" );

    const auto derived = DerivedHandlesUnder( content );

    // One reference that resolves and one that does not, in one file, so the test also shows the checker
    // does not simply reject everything. The good one is T_Checker's id, taken from its file rather than
    // written down — a literal here would be a second copy of the number this suite exists to stop having
    // two of.
    const auto good = HandleOfContentFile( content / "Textures" / "T_Checker.detex" );
    ASSERT_NE( derived.find( good ), derived.end() ) << "T_Checker.detex is missing from the checkout";

    uint64_t bad = good ^ 0x5555555555555555ull;
    while ( derived.find( bad ) != derived.end() )
        ++bad; // vanishingly unlikely, but a collision would make this test lie

    {
        std::ofstream out( scratch / "M_Dangling.demat" );
        ASSERT_TRUE( out.is_open() );
        // No `Header`, on purpose: `ReferencesUnder` reads through `rfl::json::read<MaterialData>` and
        // `Header` is optional, so this fixture stays the minimal shape the census actually needs — a
        // `Textures` array — rather than a fabricated MATL 2 document nothing here reads.
        out << R"({"Params":[],"Textures":[{"Name":"u_AlbedoTexture","TextureHandle":)" << good
            << R"(},{"Name":"u_NormalTexture","TextureHandle":)" << bad << R"(}]})";
    }

    std::string parseError;
    const auto  references = ReferencesUnder( scratch, &parseError );
    const auto  unresolved = Unresolved( references, derived );

    fs::remove_all( scratch );

    EXPECT_TRUE( parseError.empty() ) << parseError;
    ASSERT_EQ( references.size(), 2u );
    ASSERT_EQ( unresolved.size(), 1u ) << "the census did not report the dangling reference";
    EXPECT_EQ( unresolved[0].Slot, "u_NormalTexture" );
    EXPECT_EQ( unresolved[0].Handle, bad );
    EXPECT_EQ( unresolved[0].File, "M_Dangling.demat" )
         << "the failure must name the material file, or the message is another bare number";
}

// ── One spelling for an asset reference ────────────────────────────────────────────────────────────

// A reflected `AssetHandle` field is written by ReflectionSerializer as a STRING when the caller supplied
// an asset resolver and as a raw 64-bit INTEGER when it did not — so "no asset" reaches disk as `""` from
// one caller and as `0` from another. Two types for one concept in one format is a reader's problem
// forever: `""` and `0` compare equal to nothing and to each other never.
//
// Every production caller passes a resolver today (ComponentRegistry's two, and SceneSerializer's two for
// SceneSettings — the last of which was fixed on 2026-09-05), so the string is what the engine writes. That
// is a property of four call sites, not of the format, and four call sites are exactly the kind of thing a
// fifth one joins. This is the assertion that keeps it true of the FILES.
//
// The field names are read out of the generated reflection rather than typed here, for the reason
// SettingConsumers reads its source text: a hand-written list of the names to check is a second census that
// falls behind the first, and it would fall behind silently — by passing.
TEST( AssetReferenceCensus, EveryAssetReferenceInShippedContentIsSpelledAsAString )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const fs::path generated = root + "Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp";
    ASSERT_TRUE( fs::exists( generated ) ) << generated.string() << " is missing";

    // name -> the set of FieldTypes the generated reflection declares it under.
    std::map<std::string, std::set<std::string>> typesByName;
    {
        const std::string text       = ReadAll( generated );
        const std::string namePrefix = ".Name = \"";
        const std::string typePrefix = "\", .Type = FieldType::";
        for ( size_t at = text.find( namePrefix ); at != std::string::npos; at = text.find( namePrefix, at + 1 ) )
        {
            const size_t nameStart = at + namePrefix.size();
            const size_t nameEnd   = text.find( '"', nameStart );
            if ( nameEnd == std::string::npos || text.compare( nameEnd, typePrefix.size(), typePrefix ) != 0 )
                continue;
            const size_t typeStart = nameEnd + typePrefix.size();
            const size_t typeEnd =
                 text.find_first_not_of( "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", typeStart );
            typesByName[text.substr( nameStart, nameEnd - nameStart )].insert(
                 text.substr( typeStart, typeEnd - typeStart ) );
        }
    }
    ASSERT_GT( typesByName.size(), 50u ) << "the generated reflection was not parsed — nothing to check";

    // A name declared under TWO field types cannot be judged from the JSON alone: `"Volume"` is the hero
    // cloud's sculpted body (AssetHandle) AND an audio source's gain (Float), and a float gain of 1 is a
    // JSON integer. Those names are excluded, and the exclusion is DERIVED — the day the collision is
    // resolved the name rejoins the census with no edit here. (It is worth resolving: two components using
    // one word for two unrelated things is how a reader of a `.desce` guesses wrong.)
    std::set<std::string> handleNames;
    std::set<std::string> ambiguous;
    for ( const auto& [name, types] : typesByName )
    {
        if ( types.count( "AssetHandle" ) == 0 )
            continue;
        ( types.size() == 1 ? handleNames : ambiguous ).insert( name );
    }
    ASSERT_FALSE( handleNames.empty() ) << "no unambiguous AssetHandle field names were found";

    // Recursive walk of one parsed document, reporting every offending occurrence rather than the first.
    std::vector<std::string>                                       offences;
    std::function<void( const rfl::Generic&, const std::string& )> visit =
         [&]( const rfl::Generic& node, const std::string& file )
    {
        if ( const auto object = node.to_object() )
        {
            for ( const auto& [key, value] : object.value() )
            {
                // A NUMBER specifically, not merely "not a string". A key is not owned by the field
                // census: `"Material"` is both an AssetHandle field (Terrain's slot) AND the component
                // key the inline MaterialComponent serializes under, and the latter is an OBJECT. Flagging
                // "not a string" reported 24 of those in one scene as if the format were broken. The
                // defect being held here is one concept written as two TYPES — string and integer — so
                // the integer is what the test looks for, and an object simply means the name is being
                // used for something that is not a reference at all.
                if ( handleNames.count( key ) != 0 &&
                     ( value.to_int64().has_value() || value.to_double().has_value() ) )
                {
                    offences.push_back( file + ": '" + key + "' is written as a number, not a string" );
                }
                visit( value, file );
            }
            return;
        }
        if ( const auto array = node.to_array() )
        {
            for ( const auto& element : array.value() )
                visit( element, file );
        }
    };

    size_t documents = 0;
    for ( const char* subdir : { "Scenes", "Prefabs" } )
    {
        const fs::path dir = root + "Editor/Resources/Assets/" + subdir;
        if ( !fs::exists( dir ) )
            continue;
        for ( const auto& entry : fs::recursive_directory_iterator( dir ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const auto extension = entry.path().extension();
            if ( extension != ".desce" && extension != ".deprefab" )
                continue;

            // `Scenes/Autosave/` is the editor's gitignored crash-recovery copy (.gitignore) — not content,
            // never migrated, and present only on a machine that has run the editor. A sweep that descends
            // into it passes in CI and fails on a developer's desk, which is the worst of both.
            bool autosave = false;
            for ( const auto& part : entry.path() )
                autosave = autosave || part == "Autosave";
            if ( autosave )
                continue;

            const auto parsed = rfl::json::read<rfl::Generic>( ReadAll( entry.path() ) );
            if ( !parsed )
                continue; // parsing is SceneVersionGate's subject, not this one
            ++documents;
            visit( parsed.value(),
                   fs::relative( entry.path(), root + "Editor/Resources/Assets" ).generic_string() );
        }
    }

    ASSERT_GT( documents, 50u ) << "only " << documents << " documents were walked — the sweep found nothing";

    std::string report;
    for ( const auto& offence : offences )
        report += "\n  " + offence;
    EXPECT_TRUE( offences.empty() )
         << offences.size() << " asset reference(s) in shipped content are written as a NUMBER:" << report
         << "\n\nAn asset reference has one spelling — the string form the asset resolver produces, with "
            "\"\" for unset. A raw integer is what ReflectionSerializer writes when its caller passes no "
            "resolver; find that caller and give it one, then re-save the file."
         << "\n(Names excluded as ambiguous, i.e. declared under more than one field type: " << ambiguous.size()
         << ")";
}

// ── The property the census depends on ─────────────────────────────────────────────────────────────

// If two different source files derived one handle, the census above would accept a reference to either
// while only one of them can ever register — a green sweep over a genuinely broken project. The tag in the
// stable key exists to make that impossible; this asserts it over the content actually shipped.
TEST( AssetReferenceCensus, NoTwoShippedContentFilesDeriveTheSameHandle )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const fs::path content = root + "Editor/Resources/Assets";
    ASSERT_TRUE( fs::exists( content ) ) << content.string() << " is missing";

    ProjectRootGuard guard;
    Common::Constants::Path::SetProjectRoot( root + "Editor", "Resources/Assets" );

    std::map<uint64_t, std::string> claimed;
    size_t                          files = 0;

    for ( const auto& entry : fs::recursive_directory_iterator( content ) )
    {
        if ( !entry.is_regular_file() )
            continue;
        ++files;
        const auto        handle  = HandleOfContentFile( entry.path() );
        const std::string name    = fs::relative( entry.path(), content ).generic_string();
        const auto [it, inserted] = claimed.emplace( handle, name );
        EXPECT_TRUE( inserted ) << "'" << it->second << "' and '" << name << "' both derive handle " << handle
                                << ". Only one of them can ever be found by a material that names it.";
    }

    EXPECT_GT( files, 100u ) << "only " << files
                             << " content files were walked — the sweep is not looking at the project";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
