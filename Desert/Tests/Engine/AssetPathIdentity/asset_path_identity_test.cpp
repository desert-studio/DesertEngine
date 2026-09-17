// ONE FILE IS ONE ASSET, WHICHEVER OF THE TWO QUESTIONS IS ASKED — a suite about a RELATION.
//
// The relation, and the whole of what this file asserts:
//
//     For any two spellings of one file, `AssetManager::CreateAsset` and `AssetManager::FindByPath`
//     return the SAME record.
//
// It is stated as an agreement rather than as two facts because both sides were individually correct
// and a unit test of either passed. `CreateAsset` deduplicated on `AssetHandle::StableKeyForPath` — the
// asset's place in the project, spelling-independent by construction. `FindByPath` scanned the records
// comparing `AssetMetadata::Filepath` VERBATIM. A stable key is a correct identity; a path compare is a
// correct path compare; the registry still answered one question two ways, and 44 call sites were
// reading the weaker answer.
//
// WHAT IT COST, measured before the fix (Г15). Every content root turns ABSOLUTE the moment a `.deproj`
// is opened (`Constants::Path::SetProjectRoot`), so `AssetPreloader` registers
// `<home>/Game/Cooked/Meshes/base.stmesh` while the scene that references it says
// `Cooked/Meshes/base.stmesh`. `FindByPath` missed, the caller called `CreateAsset`, and `CreateAsset`
// HIT — handing back the preloader's unparsed shell to a caller that believed it had just created one.
// A StaticMesh was built from 0 vertices and 0 submeshes and cached under a live handle; `Get` answered
// zero submeshes 91 times in one 90-frame run and the frame was empty. Two other sites had each already
// grown a hand-written detour around the same disagreement rather than closing it.
//
// WHY THE PROBE ASSETS ARE DEFINED HERE and no engine asset class is used: the claim is about the
// REGISTRY, not about any file format. A local type keeps the suite free of every parser, so a red line
// in it can only mean the registry's identity moved.

#include <gtest/gtest.h>

#include <Engine/Assets/AssetManager.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>

#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

using Desert::Assets::AssetKey;
using Desert::Assets::AssetManager;
using Desert::Assets::AssetMetadata;
using Desert::Assets::AssetPriority;
using Desert::Assets::AssetTypeID;

namespace
{
    // Restores the project root SetProjectRoot rewrites. The content directories are process-wide state,
    // so a test that opens a project and walks away leaves every test after it measuring that project.
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

    // The working directory is what turns a RELATIVE spelling into a place, so the reproducer below has
    // to control it: `StableKeyForPath` resolves a relative path through `fs::absolute`, and a scene
    // saying `Cooked/Meshes/base.stmesh` means "under the project I am open in".
    class WorkingDirectoryGuard
    {
    public:
        explicit WorkingDirectoryGuard( const std::filesystem::path& moveTo )
             : m_Saved( std::filesystem::current_path() )
        {
            std::filesystem::current_path( moveTo );
        }

        ~WorkingDirectoryGuard()
        {
            std::error_code ec;
            std::filesystem::current_path( m_Saved, ec );
        }

        WorkingDirectoryGuard( const WorkingDirectoryGuard& )            = delete;
        WorkingDirectoryGuard& operator=( const WorkingDirectoryGuard& ) = delete;

    private:
        std::filesystem::path m_Saved;
    };

    // Two asset classes with DIFFERENT type ids, because the identity is (file, type) and the suite has
    // to be able to tell the two halves apart. Neither reads a byte: `Load` only flips the flag the
    // registry's own contract is written in terms of.
    template <AssetTypeID TypeId>
    class ProbeAsset final : public Desert::Assets::AssetBase
    {
    public:
        ProbeAsset( const AssetPriority priority, const Common::Filepath& filepath )
             : AssetBase( priority, filepath, GetTypeID() )
        {
        }

        static AssetTypeID GetTypeID()
        {
            return TypeId;
        }

        Common::BoolResultStr LoadFromFile() override
        {
            m_Ready = true;
            return BOOLSUCCESS;
        }

        Common::BoolResultStr Unload() override
        {
            m_Ready = false;
            return BOOLSUCCESS;
        }

        bool IsReadyForUse() const override
        {
            return m_Ready;
        }

    private:
        bool m_Ready = false;
    };

    using TextureProbe = ProbeAsset<AssetTypeID::Texture2D>;
    using SkyboxProbe  = ProbeAsset<AssetTypeID::Skybox>;

    std::filesystem::path ScratchRoot()
    {
        return std::filesystem::temp_directory_path() / "desert_asset_path_identity";
    }

    // A project on disk, opened. Nothing is written into it: the registry indexes paths and never reads
    // one, which is exactly why a spelling could disagree with an identity without anything failing.
    //
    // CANONICAL, and that is a finding rather than fixture hygiene. `StableKeyForPath` resolves a relative
    // path with `fs::absolute`, which is purely LEXICAL — it prepends the working directory and does not
    // follow a link. On macOS `temp_directory_path()` answers `/var/folders/...` while `current_path()`
    // after a chdir there answers `/private/var/folders/...`, because `/var` is a symlink; a project
    // opened under one spelling and a relative reference resolved under the other are then two
    // identities for one file. Canonicalising here makes the fixture measure the registry instead of the
    // link, and the property it sidesteps is stated in
    // `RelativeSpellingsAreResolvedLexicallyNotThroughLinks` below so that it is asserted rather than
    // merely avoided.
    std::filesystem::path OpenProject( const char* who )
    {
        std::filesystem::path dir = ScratchRoot() / who;
        std::filesystem::create_directories( dir );
        dir = std::filesystem::canonical( dir );
        Common::Constants::Path::SetProjectRoot( dir, "Resources/Assets" );
        return dir;
    }

    // EVERY SPELLING OF ONE FILE THAT THIS ENGINE ACTUALLY PRODUCES, for the file `Meshes/base.stmesh`
    // under the cooked root. They are not decorative: each one is a form some caller holds.
    //
    //   [0] the absolute one AssetPreloader registers, because the roots go absolute with a project open
    //   [1] the same place reached through a sibling directory — what `lexically_normal` exists for, and
    //       what a `..` in a stored reference produces
    //   [2] the same place with a redundant `./`, which path concatenation produces on its own
    //   [3] the project-relative one a `.desce` carries, resolved against the working directory
    //
    // The last is the one Г15 died on and the only one that needs the working directory to be the
    // project; the first three are lexical and hold anywhere.
    std::vector<Common::Filepath> SpellingsOfOneFile()
    {
        const std::filesystem::path cooked = Common::Constants::Path::COOKED_PATH;
        return {
             cooked / "Meshes" / "base.stmesh",
             cooked / "Textures" / ".." / "Meshes" / "base.stmesh",
             cooked / "." / "Meshes" / "base.stmesh",
             std::filesystem::path( "Cooked" ) / "Meshes" / "base.stmesh",
        };
    }
} // namespace

// THE LOAD-BEARING ONE. Registered under one spelling, asked for under every other — and asked with
// `FindByPath` ALONE, with no `CreateAsset` behind it to paper over a miss.
//
// The census taken before the fix found 8 of the 44 call sites in exactly this position: a miss is the
// final answer there, and the reference is dropped (`EntitySerializer` adds no PrefabComponent,
// `PrefabFactory` skips the nested body) or an error is logged about a file that is loaded
// (`CloudTypeAsset` names a noise volume that IS registered and falls back to the built-in sky).
TEST( AssetPathIdentity, EverySpellingOfOneFileFindsTheAssetRegisteredUnderAnother )
{
    const ProjectRootGuard      roots;
    const auto                  project = OpenProject( "find_only" );
    const WorkingDirectoryGuard cwd( project );

    AssetManager mgr;

    const auto spellings = SpellingsOfOneFile();
    const auto registered =
         mgr.CreateAsset<TextureProbe>( AssetPriority::Low, spellings.front(), /*loadAfterCreate=*/false );
    ASSERT_NE( registered, nullptr );

    for ( const auto& spelling : spellings )
    {
        EXPECT_EQ( mgr.FindByPath<TextureProbe>( spelling ).get(), registered.get() )
             << "registered as '" << spellings.front().generic_string() << "', asked for as '"
             << spelling.generic_string() << "'";
    }
}

// THE RELATION ITSELF, and the reason this suite is not two suites. Asserting that `FindByPath` resolves
// a spelling would go green again the day somebody gives `CreateAsset` a different key; asserting that
// the two AGREE cannot, whichever of them moves.
TEST( AssetPathIdentity, TheTwoEntryPointsAnswerOneQuestionTheSameWay )
{
    const ProjectRootGuard      roots;
    const auto                  project = OpenProject( "agreement" );
    const WorkingDirectoryGuard cwd( project );

    AssetManager mgr;

    for ( const auto& spelling : SpellingsOfOneFile() )
    {
        const auto viaCreate =
             mgr.CreateAsset<TextureProbe>( AssetPriority::Low, spelling, /*loadAfterCreate=*/false );
        const auto viaFind = mgr.FindByPath<TextureProbe>( spelling );

        ASSERT_NE( viaCreate, nullptr ) << spelling.generic_string();
        EXPECT_EQ( viaCreate.get(), viaFind.get() )
             << "CreateAsset and FindByPath disagree about '" << spelling.generic_string() << "'";
    }

    // ...and the agreement is that there is ONE asset, not that both keep finding whatever they made.
    EXPECT_EQ( mgr.RegisteredAssets().size(), 1u );
}

// Г15'S OWN SCENARIO, end to end and in its own vocabulary: the preloader registers an UNPARSED shell
// under the absolute spelling, the scene asks for the project-relative one.
//
// The assertion is on the SHELL and not merely on non-null. What the miss produced was not a null — it
// was a caller that went on to `CreateAsset`, got this very shell back, and registered it with the mesh
// service believing it had just made it. So what has to be true is that the scene's question reaches the
// shell DIRECTLY, before any create-on-miss is reached at all.
TEST( AssetPathIdentity, TheScenesSpellingFindsThePreloadersUnparsedShell )
{
    const ProjectRootGuard      roots;
    const auto                  project = OpenProject( "preloader_shell" );
    const WorkingDirectoryGuard cwd( project );

    AssetManager mgr;

    const Common::Filepath preloaderSpelling = Common::Constants::Path::COOKED_PATH / "Meshes" / "base.stmesh";
    const Common::Filepath sceneSpelling     = std::filesystem::path( "Cooked" ) / "Meshes" / "base.stmesh";

    const auto shell =
         mgr.CreateAsset<TextureProbe>( AssetPriority::Low, preloaderSpelling, /*loadAfterCreate=*/false );
    ASSERT_NE( shell, nullptr );
    ASSERT_FALSE( shell->IsReadyForUse() ) << "the fixture is meant to be an UNPARSED shell";

    const auto found = mgr.FindByPath<TextureProbe>( sceneSpelling );
    ASSERT_NE( found, nullptr ) << "the scene's spelling did not reach the preloader's record";
    EXPECT_EQ( found.get(), shell.get() );
    EXPECT_FALSE( found->IsReadyForUse() )
         << "the caller must be able to see that this record has never been parsed";
}

// The registry's own record and the question asked of it are the same identity. This is the pair that
// silently disagreed — the record kept its Filepath as spelled, the lookup key was derived — so it is
// worth asserting directly and not only through the lookups.
TEST( AssetPathIdentity, TheRecordsOwnKeyEqualsTheKeyOfEverySpellingOfIt )
{
    const ProjectRootGuard      roots;
    const auto                  project = OpenProject( "record_key" );
    const WorkingDirectoryGuard cwd( project );

    AssetManager mgr;

    const auto spellings = SpellingsOfOneFile();
    ASSERT_NE( mgr.CreateAsset<TextureProbe>( AssetPriority::Low, spellings.front(),
                                              /*loadAfterCreate=*/false ),
               nullptr );

    ASSERT_EQ( mgr.RegisteredAssets().size(), 1u );
    const AssetMetadata& record = mgr.RegisteredAssets().front().first;

    for ( const auto& spelling : spellings )
    {
        EXPECT_TRUE( AssetKey( record ) == AssetKey( spelling, AssetTypeID::Texture2D ) )
             << "record key '" << AssetKey( record ).Value() << "' vs question key '"
             << AssetKey( spelling, AssetTypeID::Texture2D ).Value() << "'";
    }
}

// A SPELLING IS NOT AN IDENTITY, BUT A FILE STILL IS: two different files must not collapse into one
// just because the comparison stopped being literal. The negative half of the relation.
TEST( AssetPathIdentity, TwoDifferentFilesStayTwoAssets )
{
    const ProjectRootGuard roots;
    OpenProject( "distinct_files" );

    AssetManager mgr;

    const Common::Filepath a = Common::Constants::Path::COOKED_PATH / "Meshes" / "base.stmesh";
    const Common::Filepath b = Common::Constants::Path::COOKED_PATH / "Meshes" / "other.stmesh";

    const auto first  = mgr.CreateAsset<TextureProbe>( AssetPriority::Low, a, /*loadAfterCreate=*/false );
    const auto second = mgr.CreateAsset<TextureProbe>( AssetPriority::Low, b, /*loadAfterCreate=*/false );

    ASSERT_NE( first, nullptr );
    ASSERT_NE( second, nullptr );
    EXPECT_NE( first.get(), second.get() );
    EXPECT_EQ( mgr.RegisteredAssets().size(), 2u );

    EXPECT_EQ( mgr.FindByPath<TextureProbe>( a ).get(), first.get() );
    EXPECT_EQ( mgr.FindByPath<TextureProbe>( b ).get(), second.get() );
}

// THE TYPE IS HALF THE IDENTITY. Two asset classes are allowed to sit on one path — the handle
// derivation deliberately gives them the same number — and they are still two records. A lookup keyed on
// the path alone would hand a Skybox to a caller asking for a Texture, which is the failure
// `AsRequestedType` exists to refuse.
TEST( AssetPathIdentity, OnePathTwoTypesStaysTwoRecordsUnderEverySpelling )
{
    const ProjectRootGuard      roots;
    const auto                  project = OpenProject( "two_types" );
    const WorkingDirectoryGuard cwd( project );

    AssetManager mgr;

    const auto spellings = SpellingsOfOneFile();

    const auto texture =
         mgr.CreateAsset<TextureProbe>( AssetPriority::Low, spellings.front(), /*loadAfterCreate=*/false );
    const auto skybox =
         mgr.CreateAsset<SkyboxProbe>( AssetPriority::Low, spellings.back(), /*loadAfterCreate=*/false );

    ASSERT_NE( texture, nullptr );
    ASSERT_NE( skybox, nullptr );
    EXPECT_NE( static_cast<void*>( texture.get() ), static_cast<void*>( skybox.get() ) );
    EXPECT_EQ( mgr.RegisteredAssets().size(), 2u );

    // Registered under opposite ends of the spelling list, and each type still resolves from both ends.
    for ( const auto& spelling : spellings )
    {
        EXPECT_EQ( mgr.FindByPath<TextureProbe>( spelling ).get(), texture.get() ) << spelling.generic_string();
        EXPECT_EQ( mgr.FindByPath<SkyboxProbe>( spelling ).get(), skybox.get() ) << spelling.generic_string();
    }
}

// A MISS IS STILL A MISS. The fix widened what counts as a hit; it must not have made the lookup answer
// for files nobody registered, which would turn "there is no such asset" into a plausible wrong pointer.
TEST( AssetPathIdentity, AFileNobodyRegisteredIsStillNotFound )
{
    const ProjectRootGuard roots;
    OpenProject( "miss" );

    AssetManager mgr;

    ASSERT_NE( mgr.CreateAsset<TextureProbe>( AssetPriority::Low,
                                              Common::Constants::Path::COOKED_PATH / "Meshes" / "base.stmesh",
                                              /*loadAfterCreate=*/false ),
               nullptr );

    EXPECT_EQ( mgr.FindByPath<TextureProbe>( Common::Constants::Path::COOKED_PATH / "Meshes" /
                                             "never_registered.stmesh" ),
               nullptr );
    EXPECT_EQ( mgr.FindByPath<TextureProbe>( "" ), nullptr );
}

// THE LIMIT OF THE IDENTITY, PINNED SO IT CANNOT MOVE IN SILENCE. A path under NO content root keeps its
// normalized spelling as its key (`StableKeyForPath` says so, and must: a file genuinely outside the
// project has no project-relative identity, and the synthetic `procedural://` keys are identities rather
// than locations). So LEXICAL spellings of an outside file still agree, and a relative-versus-absolute
// pair does not.
//
// This is not a defect of the fix — `CreateAsset` behaved exactly this way before it, which is why the
// two now agree here as well — but it is where this defect class would next appear, and a test is the
// only form of that sentence anybody will find.
TEST( AssetPathIdentity, OutsideEveryContentRootOnlyLexicalSpellingsAgree )
{
    const ProjectRootGuard roots;
    OpenProject( "outside" );

    AssetManager mgr;

    const std::filesystem::path outside = ScratchRoot() / "elsewhere" / "loose.stmesh";

    const auto registered =
         mgr.CreateAsset<TextureProbe>( AssetPriority::Low, outside, /*loadAfterCreate=*/false );
    ASSERT_NE( registered, nullptr );

    // Lexically different, same place: these agree, because normalization is spelling-independent.
    EXPECT_EQ( mgr.FindByPath<TextureProbe>( ScratchRoot() / "elsewhere" / "." / "loose.stmesh" ).get(),
               registered.get() );
    EXPECT_EQ( mgr.FindByPath<TextureProbe>( ScratchRoot() / "x" / ".." / "elsewhere" / "loose.stmesh" ).get(),
               registered.get() );

    // And the stated limit: with no root to be relative to, a relative spelling is a different identity.
    EXPECT_EQ( Common::AssetHandle::StableKeyForPath( outside ), outside.generic_string() )
         << "a path outside every content root must keep its normalized spelling as its key";
}

// A RELATIVE SPELLING IS RESOLVED LEXICALLY, NOT THROUGH LINKS — found while writing this suite, and
// asserted here because avoiding it in the fixture would have buried it.
//
// `StableKeyForPath` turns a relative path into a place with `fs::absolute`, which prepends the working
// directory and follows nothing. So a project whose root is recorded as `/var/folders/…/P` and a working
// directory that reports itself as `/private/var/folders/…/P` — the SAME directory on macOS, where
// `/var` is a symlink — give one file two identities. The first draft of this suite hit it and read as a
// defect in the fix.
//
// It is stated rather than fixed. Making the derivation canonical would put a `stat` per root inside the
// key, on the path where computing the key inside a scan already cost 56.9 s over a 2000-asset preload;
// and it would make an asset's identity depend on what the filesystem looks like at that instant, which
// is exactly what a handle written into a committed scene must not do. What WOULD change the answer: a
// project whose recorded root and whose working directory are reached by different links. The editor
// derives both from the same `.deproj` path, so this is a hazard for tooling, not for the editor.
TEST( AssetPathIdentity, RelativeSpellingsAreResolvedLexicallyNotThroughLinks )
{
    const ProjectRootGuard roots;

    std::filesystem::path dir = ScratchRoot() / "lexical";
    std::filesystem::create_directories( dir );

    const std::filesystem::path canonical = std::filesystem::canonical( dir );
    if ( canonical == dir )
    {
        GTEST_SKIP() << "the scratch directory is reached by no link on this platform, so the two "
                        "spellings of it are the same string and there is nothing to measure";
    }

    // The project is opened under the UNCANONICAL spelling, the caller stands in the canonical one.
    Common::Constants::Path::SetProjectRoot( dir, "Resources/Assets" );
    const WorkingDirectoryGuard cwd( canonical );

    AssetManager mgr;

    const auto registered = mgr.CreateAsset<TextureProbe>(
         AssetPriority::Low, Common::Constants::Path::COOKED_PATH / "Meshes" / "base.stmesh",
         /*loadAfterCreate=*/false );
    ASSERT_NE( registered, nullptr );

    EXPECT_EQ( mgr.FindByPath<TextureProbe>( std::filesystem::path( "Cooked" ) / "Meshes" / "base.stmesh" ),
               nullptr )
         << "the derivation has become link-aware; that is a bigger change than it looks (it puts a stat "
            "inside the identity of every asset) and this test is where to argue it";
}

// THE OLDER QUESTION IS NOT MERELY DISCOURAGED, IT DOES NOT COMPILE.
//
// The brief asked for the stronger thing: arrange the type so the wrong question cannot be asked. A
// string-keyed map is the weaker shape — it accepts a raw path spelled into it, which compiles, looks
// right and is the exact mistake being retired. `AssetKey` has one constructor and it takes the two
// things an identity is made of, so `m_PathLookup.find( somePath )` is a type error rather than a
// silently different answer. These are the assertions that would go red if that were relaxed.
TEST( AssetPathIdentity, AnIdentityCannotBeBuiltFromASpellingAlone )
{
    static_assert( std::is_constructible_v<AssetKey, Common::Filepath, AssetTypeID>,
                   "an identity is a file AND a type" );

    static_assert( !std::is_constructible_v<AssetKey, std::string>,
                   "a raw string must not be able to become a registry key" );
    static_assert( !std::is_constructible_v<AssetKey, const char*>,
                   "a raw string must not be able to become a registry key" );
    static_assert( !std::is_constructible_v<AssetKey, Common::Filepath>,
                   "a path alone is not an identity: two asset classes may share one path" );
    static_assert( !std::is_convertible_v<AssetMetadata, AssetKey>,
                   "the metadata conversion is explicit, so a record cannot become a key by accident" );

    SUCCEED();
}

// A PATH RELATIVE TO THE ASSETS ROOT IS NOT A SPELLING OF THE FILE, AND THE REGISTRY CANNOT MAKE IT ONE.
//
// This is the half of the identity that the suite above does NOT give you, and a task was opened on the
// belief that it did: `AssetKey` reduces every spelling of one file to one key, so surely a bare
// `Clouds/CloudNoise_FineWisp.dcnv` finds the volume the preloader registered. It does not, and the
// reason is one line of `StableKeyForPath`: a relative spelling is resolved through `fs::absolute`,
// which prepends the WORKING DIRECTORY. It is `<cwd>/Clouds/...` that is then matched against the
// content roots, and that place is under none of them — so the key stays the untagged, normalised
// spelling while the registered file's key is `assets:Clouds/...`. Two identities, one file.
//
// WHY THIS IS NOT A DEFECT IN THE DERIVATION. The engine has exactly one place that may join a
// root-relative reference to its root, and which root it is depends on the FORMAT that stored it: a
// `.decloudtype` stores its noise volume relative to ASSETS_PATH, a `.desce` stores paths relative to
// the project. A registry that guessed would have to try every root and would then answer a file under
// `Cooked/` when asked for one under `Resources/Assets/`. The join belongs to the reader of the format,
// and this test exists so that "the key is spelling-independent" is never again read as "the key is
// root-independent".
//
// WHO PAID FOR IT: Editor/Source/Editor/Panels/Clouds/CloudsPanel.cpp handed the stored spelling to
// `FindByPath` verbatim, so the Clouds window's noise stage reported "the built-in volume" for the one
// shipped cloud type that names a file. `Assets::CloudTypeAsset::ResolveDependencies` does the single
// join and is what the panel now reads back.
TEST( AssetPathIdentity, ARootRelativeReferenceIsAnotherIdentityUntilItsOwnFormatJoinsIt )
{
    const ProjectRootGuard      roots;
    const auto                  project = OpenProject( "assets_root_relative" );
    const WorkingDirectoryGuard cwd( project );

    // Exactly what the shipped `Cirrus.decloudtype` carries in its "NoiseVolume" field, and exactly where
    // the file sits. Spelled here rather than read off disk because the claim is about the KEY.
    const std::filesystem::path stored( "Clouds/CloudNoise_FineWisp.dcnv" );
    const Common::Filepath      rooted = ( Common::Constants::Path::ASSETS_PATH / stored ).lexically_normal();

    AssetManager mgr;
    const auto registered = mgr.CreateAsset<TextureProbe>( AssetPriority::Low, rooted, /*loadAfterCreate=*/false );
    ASSERT_NE( registered, nullptr );

    // The two keys are the finding, and they are asserted BEFORE the lookups so that a red line here says
    // which of the two halves moved.
    EXPECT_EQ( Common::AssetHandle::StableKeyForPath( rooted ), "assets:Clouds/CloudNoise_FineWisp.dcnv" );
    EXPECT_EQ( Common::AssetHandle::StableKeyForPath( stored ), "Clouds/CloudNoise_FineWisp.dcnv" )
         << "a path the working directory does not put under a content root keeps its own spelling";
    EXPECT_NE( Common::AssetHandle::StableKeyForPath( rooted ), Common::AssetHandle::StableKeyForPath( stored ) );

    EXPECT_EQ( mgr.FindByPath<TextureProbe>( stored ), nullptr )
         << "the bare root-relative spelling must NOT resolve: if it starts to, the join in "
            "CloudTypeAsset::ResolveDependencies has become a second answer to a question the registry "
            "now answers, and one of the two has to go";
    EXPECT_EQ( mgr.FindByPath<TextureProbe>( rooted ).get(), registered.get() )
         << "the joined spelling is the one the registry holds";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
