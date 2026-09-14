// The packaging <-> scanning relation. Constants.hpp declared FONTS_PATH and ICONS_PATH, the font
// and icon services scanned them, and the game packager packed three OTHER trees — so a packaged
// game shipped without a single .ttf and the first frame with text had nothing to draw with. Both
// ends were individually "correct"; the missing property was the RELATION between what the packager
// puts into Content.dpak and what the runtime scanners go looking for. That relation is what this
// suite asserts, three ways:
//
//   1. Census: every root the scanners enumerate is a tree the packager packs.
//   2. Keys: every packed tree's archive key prefix is exactly the key a runtime lookup of that
//      tree produces under the package root — remapped trees through the regenerated .deproj's
//      AssetsRoot, resource trees through their own (never-remapped) relative paths.
//   3. End to end: BuildContentPak() over a real (temp) project, the pak mounted in a bare
//      "package" directory, and the scanners' own enumeration finding the font, the icon and the
//      scene inside it.

#include <Editor/Packaging/GamePackager.hpp>
#include <Editor/Packaging/PackageCook.hpp>
#include <Editor/Packaging/PackageTarget.hpp>
#include <Editor/Packaging/PackagedContentTrees.hpp>

#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Project/ProjectContext.hpp>
#include <Engine/Runtime/Services/ServiceScanRoots.hpp>
#include <Engine/Text/FontBaker.hpp>
#include <Engine/Text/FontCache.hpp>
#include <Engine/Vector/IconBake.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/ContentManifest.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>
#include <Common/Utilities/VFS.hpp>

// The Runtime's own startup discovery — the other end of the relation П5 closes.
#include <PackagedContent.hpp>

#include <gtest/gtest.h>

#include <cstdint>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../../TestSupport/result_assert.hpp"

namespace fs = std::filesystem;

namespace
{
    void WriteFile( const fs::path& p, const std::string& content )
    {
        fs::create_directories( p.parent_path() );
        std::ofstream out( p, std::ios::binary );
        out << content;
    }

    // The archive key a lookup of `dir` produces once the VFS normalizes it against the package
    // root — the same relation VFS::KeyFor implements.
    std::string KeyUnder( const fs::path& root, const fs::path& dir )
    {
        const fs::path rel = dir.lexically_normal().lexically_relative( root.lexically_normal() );
        std::string    key;
        for ( const auto& part : rel )
        {
            if ( part.empty() || part == "." )
                continue;
            if ( !key.empty() )
                key += '/';
            key += part.generic_string();
        }
        return key;
    }

    void SetEnv( const char* key, const std::string& value )
    {
#if defined( _WIN32 )
        _putenv_s( key, value.c_str() );
#else
        setenv( key, value.c_str(), 1 );
#endif
    }

    // Restores cwd, HOME and the (global) project-root remap, whatever the test body did.
    struct EnvironmentGuard
    {
        fs::path    OldCwd  = fs::current_path();
        std::string OldHome = std::getenv( "HOME" ) ? std::getenv( "HOME" ) : "";
        ~EnvironmentGuard()
        {
            std::error_code ec;
            fs::current_path( OldCwd, ec );
            if ( !OldHome.empty() )
                SetEnv( "HOME", OldHome );
            Common::Utils::VFS::Unmount();
            // Back to the built-in sandbox mapping the process started with.
            Common::Constants::Path::SetProjectRoot( "", "Resources/Assets" );
        }
    };
} // namespace

TEST( PackagedContent, EveryScannedRootIsAPackagedTree )
{
    const auto trees = Desert::Editor::PackagedContentTrees();

    // Pointer identity, not path equality: the scanners and the packager must read the SAME live
    // constant, so a project remap can never split the two.
    const auto packed = [&]( const fs::path* root )
    {
        for ( const auto& t : trees )
            if ( t.Tree == root )
                return true;
        return false;
    };

    for ( const fs::path* root : Desert::Runtime::FontScanRoots() )
        EXPECT_TRUE( packed( root ) ) << "font scan root not packaged: " << root->string();
    for ( const fs::path* root : Desert::Runtime::IconScanRoots() )
        EXPECT_TRUE( packed( root ) ) << "icon scan root not packaged: " << root->string();
}

TEST( PackagedContent, PakKeysAreTheRuntimeLookupKeysUnderThePackageRoot )
{
    EnvironmentGuard guard;

    // Simulate the packaged game's world: Game.deproj opened from the package dir remaps the content
    // trees under it, the launcher cds there, and every resource path resolves against it.
    const fs::path pkg = fs::temp_directory_path() / "desert_pkgkeys";
    Common::Constants::Path::SetProjectRoot( pkg, Desert::Editor::kPackagedAssetsRoot );

    for ( const auto& t : Desert::Editor::PackagedContentTrees() )
    {
        const fs::path lookup = t.Tree->is_absolute() ? *t.Tree : pkg / *t.Tree;
        EXPECT_EQ( KeyUnder( pkg, lookup ), std::string( t.PakKey ) )
             << "tree " << t.Tree->string() << " is packed under a key its own lookup cannot reach";
    }
}

TEST( PackagedContent, BuildContentPakPacksWhatTheScannersFind )
{
    EnvironmentGuard guard;

    const fs::path base = fs::temp_directory_path() / "desert_pkg_e2e";
    fs::remove_all( base );
    const fs::path proj = base / "proj";
    const fs::path pkg  = base / "pkg";

    // ---- the DEV side: a project with one scene, plus engine resources next to the editor's cwd.
    // AssetsRoot is deliberately NOT "Assets", so the test also proves the packer rebases content
    // into the packaged root rather than echoing the dev layout.
    WriteFile( proj / "GameAssets" / "Scenes" / "level.desce", "scene-body" );
    WriteFile( proj / "Resources" / "Fonts" / "fake.ttf", "font-body" );
    WriteFile( proj / "Resources" / "Icons" / "fake.svg", "icon-body" );
    WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":\"\"}" );

    SetEnv( "HOME", base.string() ); // keep RegisterRecent out of the real user config
    fs::current_path( proj );        // relative resource trees resolve against the editor cwd
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    const auto result = Desert::Editor::BuildContentPak();
    ASSERT_TRUE( result.Success ) << result.Message;

    // ---- the PACKAGED side: a bare directory holding ONLY the pak and the regenerated descriptor —
    // no loose content at all, exactly what a player's machine has.
    fs::create_directories( pkg );
    fs::copy_file( proj / "Content.dpak", pkg / "Content.dpak" );
    // The descriptor is NOT written here: it comes out of the archive, which is where the packer put
    // it. This block used to hand-splice one, which is exactly how the packer and the player managed
    // to disagree about its name for as long as they did (П5).

    fs::current_path( pkg );
    const auto mounted = Common::Utils::VFS::MountPak( pkg / "Content.dpak" );
    ASSERT_TRUE( mounted.IsSuccess() ) << mounted.GetError();
    ASSERT_TRUE(
         Desert::Project::ProjectContext::Open( ( pkg / Desert::Project::kPackagedDescriptorName ).string() ) );

    // The scanners' own enumeration: roots from ServiceScanRoots, both halves via ListFilesRecursive.
    const auto findByExt = []( const std::array<const fs::path*, 2>& roots, const char* ext )
    {
        std::vector<fs::path> out;
        for ( const fs::path* root : roots )
            for ( const auto& p : Common::Utils::FileSystem::ListFilesRecursive( *root ) )
                if ( p.extension() == ext )
                    out.push_back( p );
        return out;
    };

    const auto fonts = findByExt( Desert::Runtime::FontScanRoots(), ".ttf" );
    ASSERT_EQ( fonts.size(), 1u ) << "the packed font tree is invisible to the font scan";
    EXPECT_EQ( fonts[0].filename(), "fake.ttf" );
    // ...and the path the scan produced actually READS, which is what FontService::Get does next.
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( fonts[0] ), "font-body" );

    const auto icons = findByExt( Desert::Runtime::IconScanRoots(), ".svg" );
    ASSERT_EQ( icons.size(), 1u ) << "the packed icon tree is invisible to the icon scan";
    EXPECT_EQ( icons[0].filename(), "fake.svg" );

    // Project content went in under the packaged AssetsRoot and comes back out of the remapped root.
    const auto assets = Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::ASSETS_PATH );
    ASSERT_EQ( assets.size(), 1u );
    EXPECT_EQ( assets[0].filename(), "level.desce" );
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( assets[0] ), "scene-body" );
}

// ── A SCRIPT REFERENCE NAMES ONE FILE, LOOSE AND PACKAGED (I9) ───────────────────────────────────────
//
// THE RELATION, and it is a relation rather than a property of either side: the string a scene stores to
// name its `.lua` must resolve, in the development tree and in a mounted archive, to THE SAME FILE.
// Asserting only that it resolves in the editor is what let the defect live — that half was always true.
//
// WHAT WAS WRONG. Every other reference in a `.desce` is an AssetHandle hashed from `<tag>:<path relative
// to that root>`, so it survives the packager rebasing content under <package>/Assets/. A script slot was
// the one kind of content that named ITSELF with the rooted spelling the editor happened to be standing
// in, and a rooted spelling does not survive the rebase. I8 measured it on a mounted archive: the stored
// spelling gave Exists=0, the same file through the scripts root gave Exists=1. This is that measurement,
// turned into a suite, and it carries BOTH halves — the negative control below is the pre-migration
// spelling, and it must still fail, because a silence proves nothing until the noise is shown.
//
// The AssetsRoot is deliberately NOT "Assets", so the packaged root genuinely differs from the dev one
// and a test that merely echoed the dev layout could not pass.
TEST( PackagedContent, AScriptReferenceResolvesToTheSameFileLooseAndPackaged )
{
    EnvironmentGuard guard;

    const fs::path base = fs::temp_directory_path() / "desert_pkg_script_ref";
    fs::remove_all( base );
    const fs::path proj = base / "proj";
    const fs::path pkg  = base / "pkg";

    const std::string body = "-- MoveAlongX\nProperties = { Speed = 3 }\n";
    WriteFile( proj / "GameAssets" / "Scripts" / "Examples" / "MoveAlongX.lua", body );
    WriteFile( proj / "GameAssets" / "Scenes" / "level.desce", "scene-body" );
    WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":\"\"}" );

    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    // ---- the DEV side. The reference is minted exactly the way the Details panel's script picker mints
    // it: enumerate the census row for scripts, then StableKeyForPath over what the enumeration returned.
    std::vector<fs::path> found;
    for ( const auto& p : Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::SCRIPT_PATH ) )
        if ( p.extension() == ".lua" )
            found.push_back( p );
    ASSERT_EQ( found.size(), 1u ) << "the scripts census row does not see the project's own script";

    const std::string stored = Common::AssetHandle::StableKeyForPath( found[0] );
    EXPECT_EQ( stored, "assets:Scripts/Examples/MoveAlongX.lua" )
         << "the stored form must be root-tagged and relative, or it cannot survive the rebase";

    const fs::path loosePath = Common::AssetHandle::PathForStableKey( stored );
    ASSERT_TRUE( Common::Utils::FileSystem::Exists( loosePath ) ) << loosePath.string();
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( loosePath ), body );

    // Key -> path -> identity round-trips under THIS root: resolving the reference and re-deriving an
    // asset identity from what came back gives the identity the reference itself hashes to.
    EXPECT_EQ( Common::AssetHandle::FromCookedPath( loosePath ), Common::AssetHandle::FromKey( stored ) );

    // The spelling a v15 scene carried: the file as seen from the editor's working directory. Kept so the
    // negative control below is the ACTUAL old value and not an invented one.
    const fs::path preMigrationSpelling = fs::relative( loosePath, proj );
    ASSERT_FALSE( preMigrationSpelling.empty() );
    EXPECT_TRUE( Common::Utils::FileSystem::Exists( preMigrationSpelling ) )
         << "the old spelling resolved in the dev tree - that half was never the defect";

    // ---- the PACKAGED side: a bare directory holding only the archive and the regenerated descriptor.
    const auto result = Desert::Editor::BuildContentPak();
    ASSERT_TRUE( result.Success ) << result.Message;

    fs::create_directories( pkg );
    fs::copy_file( proj / "Content.dpak", pkg / "Content.dpak" );
    // The descriptor is NOT written here: it comes out of the archive, which is where the packer put
    // it. This block used to hand-splice one, which is exactly how the packer and the player managed
    // to disagree about its name for as long as they did (П5).

    fs::current_path( pkg );
    const auto mounted = Common::Utils::VFS::MountPak( pkg / "Content.dpak" );
    ASSERT_TRUE( mounted.IsSuccess() ) << mounted.GetError();
    ASSERT_TRUE(
         Desert::Project::ProjectContext::Open( ( pkg / Desert::Project::kPackagedDescriptorName ).string() ) );

    const fs::path packagedPath = Common::AssetHandle::PathForStableKey( stored );
    ASSERT_TRUE( Common::Utils::FileSystem::Exists( packagedPath ) )
         << "the stored script reference resolves to nothing in the package: " << packagedPath.string();

    // THE RELATION ITSELF: one reference, two roots, the same file.
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( packagedPath ), body );

    // ...and the identity round trip holds under the PACKAGED root too, which is what puts a script on
    // the same footing as a texture or a mesh.
    //
    // NOT `FromCookedPath(loosePath) == FromCookedPath(packagedPath)`, which is what this assertion said
    // first and which failed for an honest reason worth writing down: a path-derived handle is only
    // meaningful while the project that path belongs to is OPEN. With the packaged project open, the dev
    // tree's absolute path lies under no content root at all, so StableKeyForPath hands it back verbatim
    // and it hashes to something else (measured: 3427774758061914252 vs 2091480530661102989). The
    // invariant is between the stored KEY and whatever that key resolves to here — never between two
    // absolute paths from two different roots.
    EXPECT_EQ( Common::AssetHandle::FromCookedPath( packagedPath ), Common::AssetHandle::FromKey( stored ) );

    // ...and the test is not vacuous: the two resolutions really are different places on disk, so the
    // equality above is a property of the reference and not of the layout having stayed put.
    EXPECT_NE( loosePath, packagedPath );

    // ---- the NEGATIVE CONTROL. The pre-migration spelling, unchanged, against the same mounted archive.
    // It must NOT resolve; if it did, this whole suite would be measuring nothing.
    EXPECT_FALSE( Common::Utils::FileSystem::Exists( preMigrationSpelling ) )
         << "the rooted spelling '" << preMigrationSpelling.string()
         << "' resolved inside the package, so this test cannot tell a fixed reference from a broken one";
}

// ── A FONT AND AN ICON RESOLVE TO THE SAME FILE, LOOSE AND PACKAGED (I10) ────────────────────────────
//
// THE SAME RELATION as the script test above, for the other two references that stored a path: a font
// and a vector icon dropped into the PROJECT'S OWN assets tree. Both scan roots accept that tree
// (Runtime/Services/ServiceScanRoots.hpp), and until I10 the scene stored the path the service's registry
// held — which the packager's rebase under <package>/Assets/ breaks exactly as it broke a script slot.
//
// WHY THIS WENT UNSEEN and why the fixture below therefore uses PROJECT assets and not the engine ones:
// every font and icon this repository ships names Resources/Fonts or Resources/Icons, engine trees the
// packager stores under their own dev-time relative paths and SetProjectRoot never remaps. A test built
// from those would pass before the fix as well as after it, and would be measuring nothing.
//
// The negative control is the pre-migration spelling, and it must still fail on both.
TEST( PackagedContent, AServiceAssetReferenceResolvesToTheSameFileLooseAndPackaged )
{
    EnvironmentGuard guard;

    const fs::path base = fs::temp_directory_path() / "desert_pkg_service_ref";
    fs::remove_all( base );
    const fs::path proj = base / "proj";
    const fs::path pkg  = base / "pkg";

    // AssetsRoot deliberately NOT "Assets", so the packaged root genuinely differs from the dev one.
    //
    // The two files are SYNTHETIC BYTES, and the package cook says so out loud — "could not be baked
    // into an SDF atlas", "has no filled shapes the icon importer understands". That is the cook working
    // and it is not what this test is about: the relation under test is where a stored reference
    // RESOLVES, and the raw file travels into the archive and back whether or not its SDF could be
    // pre-baked. Real fixtures would add megabytes to the suite to change nothing it asserts.
    const std::string fontBody = "not-a-real-ttf-but-bytes-are-bytes";
    const std::string iconBody = "<svg><path d=\"M0 0 L1 1\"/></svg>";
    WriteFile( proj / "GameAssets" / "Fonts" / "Custom.ttf", fontBody );
    WriteFile( proj / "GameAssets" / "UI" / "Glyphs" / "spark.svg", iconBody );
    WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":\"\"}" );

    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    struct Case
    {
        const char* What;
        const char* Extension;
        std::string Body;
        const char* ExpectedKey;
    };
    const std::vector<Case> cases = {
         { "font", ".ttf", fontBody, "assets:Fonts/Custom.ttf" },
         { "icon", ".svg", iconBody, "assets:UI/Glyphs/spark.svg" },
    };

    // ---- the DEV side. Each reference is minted the way the services' own scan mints it: enumerate the
    // scan root through the shared enumeration, then StableKeyForPath over what came back.
    std::vector<std::string> stored;
    std::vector<fs::path>    loosePaths;
    std::vector<fs::path>    preMigrationSpellings;

    for ( const Case& c : cases )
    {
        std::vector<fs::path> found;
        for ( const auto& p :
              Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::ASSETS_PATH ) )
            if ( p.extension() == c.Extension )
                found.push_back( p );
        ASSERT_EQ( found.size(), 1u ) << c.What << ": the assets scan root does not see the project's own file";

        const std::string key = Common::AssetHandle::StableKeyForPath( found[0] );
        EXPECT_EQ( key, c.ExpectedKey ) << c.What;

        const fs::path loose = Common::AssetHandle::PathForStableKey( key );
        ASSERT_TRUE( Common::Utils::FileSystem::Exists( loose ) ) << c.What << ": " << loose.string();
        DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( loose ), c.Body );

        // The handle each service will mint for this file is FromCookedPath over the resolved path
        // (FontService::RegisterFont / IconService::RegisterIcon), and it must be the handle the STORED
        // key hashes to — otherwise a saved reference and a scanned file are two identities of one asset.
        EXPECT_EQ( Common::AssetHandle::FromCookedPath( loose ), Common::AssetHandle::FromKey( key ) ) << c.What;

        stored.push_back( key );
        loosePaths.push_back( loose );

        // What a v16 scene carried: the file as seen from the editor's working directory. Kept so the
        // negative control below is the ACTUAL old value rather than an invented one.
        preMigrationSpellings.push_back( fs::relative( loose, proj ) );
        EXPECT_TRUE( Common::Utils::FileSystem::Exists( preMigrationSpellings.back() ) )
             << c.What << ": the old spelling resolved in the dev tree - that half was never the defect";
    }

    // ---- the PACKAGED side: a bare directory holding only the archive and the regenerated descriptor.
    const auto result = Desert::Editor::BuildContentPak();
    ASSERT_TRUE( result.Success ) << result.Message;

    fs::create_directories( pkg );
    fs::copy_file( proj / "Content.dpak", pkg / "Content.dpak" );
    // The descriptor is NOT written here: it comes out of the archive, which is where the packer put
    // it. This block used to hand-splice one, which is exactly how the packer and the player managed
    // to disagree about its name for as long as they did (П5).

    fs::current_path( pkg );
    const auto mounted = Common::Utils::VFS::MountPak( pkg / "Content.dpak" );
    ASSERT_TRUE( mounted.IsSuccess() ) << mounted.GetError();
    ASSERT_TRUE(
         Desert::Project::ProjectContext::Open( ( pkg / Desert::Project::kPackagedDescriptorName ).string() ) );

    for ( std::size_t i = 0; i < cases.size(); ++i )
    {
        const Case&    c        = cases[i];
        const fs::path packaged = Common::AssetHandle::PathForStableKey( stored[i] );

        ASSERT_TRUE( Common::Utils::FileSystem::Exists( packaged ) )
             << c.What << ": the stored reference resolves to nothing in the package: " << packaged.string();

        // THE RELATION: one reference, two roots, the same file.
        DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( packaged ), c.Body );

        // The identity round trip holds under the packaged root too. NOT an equality between the two
        // resolutions' handles — that is I9's disproved hypothesis and it is false for an honest reason:
        // a path-derived handle only means anything while the project that path belongs to is open.
        EXPECT_EQ( Common::AssetHandle::FromCookedPath( packaged ), Common::AssetHandle::FromKey( stored[i] ) )
             << c.What;

        // ...and the test is not vacuous: the two resolutions really are different places on disk.
        EXPECT_NE( loosePaths[i], packaged ) << c.What;

        // ---- the NEGATIVE CONTROL: the pre-migration spelling against the same mounted archive.
        EXPECT_FALSE( Common::Utils::FileSystem::Exists( preMigrationSpellings[i] ) )
             << c.What << ": the rooted spelling '" << preMigrationSpellings[i].string()
             << "' resolved inside the package, so this test cannot tell a fixed reference from a broken one";
    }
}

// ── A PACKAGE THAT DID NOT COOK EVERYTHING SAYS SO (I12) ─────────────────────────────────────────────
//
// THE RELATION: what the cook could not put into the package and what the packaging result reports must
// be the same fact. They were free to disagree, and did — `CookStats::Failures` was counted, logged and
// then dropped, so a project with an unbakeable font packaged with `Success == true` and a message
// indistinguishable from a clean build. The Build Settings panel painted it the same green.
//
// This is Ф4's shape at the WORST possible place: the last step before a build reaches a player. Whatever
// did not make it is then discovered by whoever runs the game, not by whoever built it.
//
// BOTH DIRECTIONS, because either alone is satisfied by something useless: a result that is never
// complete would pass the first half, and one that is always complete would pass the second.
TEST( PackagedContent, ACleanProjectPackagesComplete )
{
    EnvironmentGuard guard;

    const fs::path base = fs::temp_directory_path() / "desert_pkg_complete_clean";
    fs::remove_all( base );
    const fs::path proj = base / "proj";

    // Nothing the cook can fail on: one scene, no font, no icon, no shader tree.
    WriteFile( proj / "GameAssets" / "Scenes" / "level.desce", "scene-body" );
    WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":\"\"}" );

    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    const auto result = Desert::Editor::BuildContentPak();

    ASSERT_TRUE( result.Success ) << result.Message;
    EXPECT_EQ( result.CookFailures, 0u ) << result.Message;
    EXPECT_EQ( result.CookUnwritten, 0u ) << result.Message;
    EXPECT_TRUE( result.Complete() )
         << "a project with nothing broken in it must package complete, or the incomplete verdict below "
            "is measuring the fixture rather than the cook: "
         << result.Message;
}

TEST( PackagedContent, AnAssetTheCookCannotBakeMakesThePackageIncompleteAndSaysHowMany )
{
    EnvironmentGuard guard;

    const fs::path base = fs::temp_directory_path() / "desert_pkg_complete_broken";
    fs::remove_all( base );
    const fs::path proj = base / "proj";

    // The same project as above plus ONE corrupt asset: a `.ttf` the font baker cannot read. That is the
    // whole difference between this case and the clean one, so the verdict below is attributable.
    WriteFile( proj / "GameAssets" / "Scenes" / "level.desce", "scene-body" );
    WriteFile( proj / "GameAssets" / "Fonts" / "Corrupt.ttf", "this is not a font" );
    WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":\"\"}" );

    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    const auto result = Desert::Editor::BuildContentPak();

    // A PACKAGE STILL EXISTS, and that is deliberate rather than a compromise: a project may ship content
    // that is already broken, and the packager's job is to say what it shipped, not to declare the
    // project invalid. (This used to add "this repository does, on purpose" and point at a broken shader
    // in Editor/Resources/Shaders. Г20 moved that fixture into a test tree — it was compiled at every
    // editor start — so the claim is no longer true of this repository and the fixture above, a `.ttf`
    // this suite writes itself, is what the argument now rests on.)
    EXPECT_TRUE( result.Success ) << result.Message;

    // ...but it is NOT complete, and it says how many, in a number rather than in prose.
    EXPECT_GE( result.CookFailures, 1u ) << result.Message;
    EXPECT_FALSE( result.Complete() )
         << "the cook could not bake an asset and the result still reads as a clean package: " << result.Message;

    // The prose has to agree with the number. Not because a caller should parse it — that is what the
    // counts are for — but because the message is what a human is shown, and a build result whose words
    // and numbers disagree is worse than either alone.
    EXPECT_NE( result.Message.find( "could not be cooked" ), std::string::npos ) << result.Message;
}

// ---- The COOKED-CACHE relation ------------------------------------------------------------------------
//
// П2's defect, stated as the relation these tests pin: what the packager cooks into the archive must be
// what the runtime's cache lookups read back out of it. Both halves used to be individually "correct" —
// the census packed the whole Cooked/ tree, and the runtime kept a working cache — but the lookups were
// raw ifstreams, so a packaged game (no loose Cooked/ at all) recompiled every shader and rebaked every
// atlas at every cold start, with its warm cache sitting unread in the mounted pak.

// A cooked artifact of each kind, stored through the packager's own store seam, packed by
// BuildContentPak, and read back through the runtime's own lookup — in a bare directory whose only
// content is the archive, exactly what a player's machine has.
TEST( PackagedContent, CookedArtifactsTravelFromThePackagerToTheRuntimeLookup )
{
    EnvironmentGuard guard;

    const fs::path base = fs::temp_directory_path() / "desert_pkg_cooked";
    fs::remove_all( base );
    const fs::path proj = base / "proj";
    const fs::path pkg  = base / "pkg";

    WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":\"\"}" );
    SetEnv( "HOME", base.string() );
    fs::create_directories( proj / "GameAssets" );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    // The dev side stores one artifact of each kind, exactly as the cook does.
    const std::vector<uint32_t> spirv    = { 0x07230203u, 1u, 2u, 3u };
    const uint64_t              spirvKey = 0xA5A5A5A5DEADBEEFull;
    Desert::Core::StoreCachedSpirv( spirvKey, spirv );

    Desert::Text::BakedFont font;
    font.AtlasWidth        = 2;
    font.AtlasHeight       = 2;
    // Four texels of a grey ramp with the opaque alpha the real baker writes; the values themselves
    // carry no meaning beyond "these exact bytes must come back".
    constexpr uint8_t          kOpaque = 255;
    const std::vector<uint8_t> atlas   = { 10, 10, 10, kOpaque, 20, 20, 20, kOpaque,
                                           30, 30, 30, kOpaque, 40, 40, 40, kOpaque };
    font.AtlasRGBA                     = atlas;
    font.PixelHeight       = Desert::Text::kDefaultBakePixelHeight;
    // The band the atlas was baked with: TryLoadBakedFont refuses an atlas whose band disagrees with the
    // one this build renders, so a fixture that leaves it at zero is a fixture that gets refused.
    font.DistanceRangeTexels = Desert::Text::kDistanceRangeTexels;
    const uint64_t fontKey = Desert::Text::FontCacheKey( { 1, 2, 3 }, font.PixelHeight, {} );
    Desert::Text::StoreBakedFont( Desert::Text::FontCachePath( fontKey ), font );

    Desert::Vector::BakedIcon icon;
    icon.Aspect = 2.0f;
    icon.Layers.push_back(
         { std::vector<uint8_t>( Desert::Vector::kIconCellDim * Desert::Vector::kIconCellDim, 7 ), 0x11223344u } );
    const uint64_t iconKey = Desert::Vector::IconCacheKey( { 4, 5, 6 } );
    Desert::Vector::StoreBakedIcon( Desert::Vector::IconCachePath( iconKey ), icon );

    const auto result = Desert::Editor::BuildContentPak();
    ASSERT_TRUE( result.Success ) << result.Message;

    // The packaged side: pak + descriptor, nothing loose.
    fs::create_directories( pkg );
    fs::copy_file( proj / "Content.dpak", pkg / "Content.dpak" );
    // The descriptor is NOT written here: it comes out of the archive, which is where the packer put
    // it. This block used to hand-splice one, which is exactly how the packer and the player managed
    // to disagree about its name for as long as they did (П5).
    fs::current_path( pkg );
    const auto mounted = Common::Utils::VFS::MountPak( pkg / "Content.dpak" );
    ASSERT_TRUE( mounted.IsSuccess() ) << mounted.GetError();
    ASSERT_TRUE(
         Desert::Project::ProjectContext::Open( ( pkg / Desert::Project::kPackagedDescriptorName ).string() ) );

    // The runtime's own lookups, byte for byte, with no loose Cooked/ anywhere.
    const auto loadedSpirv = Desert::Core::TryLoadCachedSpirv( spirvKey );
    ASSERT_TRUE( loadedSpirv.has_value() ) << "the packed SPIR-V cache is invisible to the runtime's cache lookup";
    EXPECT_EQ( *loadedSpirv, spirv );

    Desert::Text::BakedFont loadedFont;
    ASSERT_TRUE( Desert::Text::TryLoadBakedFont( Desert::Text::FontCachePath( fontKey ), loadedFont ) )
         << "the packed font atlas is invisible to the runtime's cache lookup";
    EXPECT_EQ( loadedFont.AtlasRGBA, font.AtlasRGBA );
    EXPECT_EQ( loadedFont.PixelHeight, font.PixelHeight );

    Desert::Vector::BakedIcon loadedIcon;
    ASSERT_TRUE( Desert::Vector::TryLoadBakedIcon( Desert::Vector::IconCachePath( iconKey ), loadedIcon ) )
         << "the packed icon bake is invisible to the runtime's cache lookup";
    ASSERT_EQ( loadedIcon.Layers.size(), 1u );
    EXPECT_EQ( loadedIcon.Layers[0].Sdf, icon.Layers[0].Sdf );
    EXPECT_EQ( loadedIcon.Layers[0].RGBA, icon.Layers[0].RGBA );
    EXPECT_EQ( loadedIcon.Aspect, icon.Aspect );

    // The archive keys, spelled BY HAND. The load/store pair above shares one path function, so a
    // mutation of that function alone (renaming "ShaderCache", dropping the "Cooked" prefix) would
    // move both ends together and stay green — these three literals are the external contract that
    // must not drift, because every already-shipped archive spells its entries this way.
    EXPECT_TRUE(
         Common::Utils::VFS::ReadFile( pkg / "Cooked" / "ShaderCache" / std::format( "{:016x}.spv", spirvKey ) )
              .has_value() );
    EXPECT_TRUE(
         Common::Utils::VFS::ReadFile( pkg / "Cooked" / "FontCache" / std::format( "{:016x}.dfont", fontKey ) )
              .has_value() );
    EXPECT_TRUE(
         Common::Utils::VFS::ReadFile( pkg / "Cooked" / "IconCache" / std::format( "{:016x}.dicon", iconKey ) )
              .has_value() );
}

// The cook produces artifacts under the very keys the runtime computes when it loads the same shader —
// over a real (minimal) DSL shader, through the real preprocessor and the real compiler. A cook that
// hashed different inputs, assembled stages differently, or keyed for the wrong profile would leave
// this lookup cold, which is precisely the shipped-cache-dead-on-arrival failure П2 was.
TEST( PackagedContent, TheCookCompilesWhatTheRuntimeWillAskFor )
{
    EnvironmentGuard guard;

    const fs::path base = fs::temp_directory_path() / "desert_pkg_cook";
    fs::remove_all( base );
    const fs::path proj = base / "proj";

    const char* kProbeShader = "Shader \"CookProbe\"\n"
                               "{\n"
                               "    Domain Surface\n"
                               "    Vertex\n"
                               "    {\n"
                               "        In(0) vec3 a_Position;\n"
                               "        void main() { gl_Position = vec4( a_Position, 1.0 ); }\n"
                               "    }\n"
                               "    Fragment\n"
                               "    {\n"
                               "        Out(0) vec4 o_Color;\n"
                               "        void main() { o_Color = vec4( 1.0 ); }\n"
                               "    }\n"
                               "}\n";

    WriteFile( proj / "Resources" / "Shaders" / "CookProbe.shader", kProbeShader );
    WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":\"\"}" );
    SetEnv( "HOME", base.string() );
    fs::create_directories( proj / "GameAssets" );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    // The packager's cook, at this build's own profile (what BuildContentPak passes).
    const auto stats = Desert::Editor::CookContentCaches( Desert::Core::SpirvDebugInfoThisBuild() );
    EXPECT_EQ( stats.ShadersCompiled, 2u ) << "vertex + fragment of the one probe shader";
    EXPECT_EQ( stats.Failures, 0u );

    // The runtime's side of the relation: assemble the same stages the way VulkanShader::Reload does
    // and ask the cache with the runtime's own key overload. Every stage must already be there.
    const fs::path shaderFile = fs::path( "Resources" ) / "Shaders" / "CookProbe.shader";

    // Ф3 made the primitive return a ResultStr. Asserting on the read ITSELF rather than on an empty
    // string is the point of that change: a probe file this test cannot read is a broken fixture and
    // must say so by name, not fail three lines later as "the shader has no stages".
    const auto contentRead = Common::Utils::FileSystem::ReadFileContent( shaderFile );
    ASSERT_TRUE( static_cast<bool>( contentRead ) ) << contentRead.GetError();
    const std::string& content = contentRead.GetValue();
    ASSERT_FALSE( content.empty() );

    const auto stages =
         Desert::Core::Preprocess::ShaderPreprocess::PreProcessProgramPass( content, shaderFile, "" );
    ASSERT_EQ( stages.size(), 2u );
    for ( const auto& [stage, source] : stages )
    {
        const uint64_t key = Desert::Core::ComputeShaderCacheKey( stage, source, shaderFile );
        EXPECT_TRUE( Desert::Core::TryLoadCachedSpirv( key ).has_value() )
             << "the cook left the " << static_cast<int>( stage )
             << " stage cold — the runtime would recompile it at startup";
    }

    // And the cook is incremental: a second pass finds everything under its key and compiles nothing.
    const auto again = Desert::Editor::CookContentCaches( Desert::Core::SpirvDebugInfoThisBuild() );
    EXPECT_EQ( again.ShadersCompiled, 0u );
    EXPECT_EQ( again.ShadersCached, 2u );
}

// A cook that could not WRITE what it produced must not report it as cooked. Without this the
// packager's own summary is the defect in miniature: it says the shader was compiled, the pak packs
// the directory that does not contain it, and the first place anyone learns otherwise is a player's
// slow startup — which is exactly the failure П2 is about, arriving one artifact at a time.
//
// The store is made to fail the way a read-only install makes it fail: the directory the artifact
// must go in cannot be created, because a FILE already occupies that name.
TEST( PackagedContent, ACookThatCannotWriteDoesNotReportTheArtifactAsCooked )
{
    EnvironmentGuard guard;

    const fs::path base = fs::temp_directory_path() / "desert_pkg_unwritable";
    fs::remove_all( base );
    const fs::path proj = base / "proj";

    WriteFile( proj / "Resources" / "Shaders" / "CookProbe.shader",
               "Shader \"CookProbe\"\n"
               "{\n"
               "    Domain Surface\n"
               "    Vertex\n"
               "    {\n"
               "        In(0) vec3 a_Position;\n"
               "        void main() { gl_Position = vec4( a_Position, 1.0 ); }\n"
               "    }\n"
               "    Fragment\n"
               "    {\n"
               "        Out(0) vec4 o_Color;\n"
               "        void main() { o_Color = vec4( 1.0 ); }\n"
               "    }\n"
               "}\n" );
    WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":\"\"}" );
    SetEnv( "HOME", base.string() );
    fs::create_directories( proj / "GameAssets" );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    // Occupy Cooked/ShaderCache with a regular file, so create_directories cannot make the folder
    // and every store into it fails.
    const fs::path cacheDir = Desert::Core::SpirvCachePathForKey( 0 ).parent_path();
    fs::create_directories( cacheDir.parent_path() );
    WriteFile( cacheDir, "not a directory" );
    ASSERT_TRUE( fs::is_regular_file( cacheDir ) );

    const auto stats = Desert::Editor::CookContentCaches( Desert::Core::SpirvDebugInfoThisBuild() );

    EXPECT_EQ( stats.ShadersCompiled, 0u ) << "an artifact that never reached the disk was counted as cooked";
    EXPECT_EQ( stats.StoreFailures, 2u ) << "vertex + fragment, each produced and each unwritten";
    EXPECT_EQ( stats.Failures, 0u ) << "the shader compiles fine — this is a WRITE failure, not a bad shader";
}

// ---- The HOST-TARGET relation -----------------------------------------------------------------------
//
// П6's other half. `PackageGame` used to state macOS in four independent places — the Runtime binary's
// name, the .app layout, the bash launcher and the build script named in its "not found" error — so on a
// Windows host it looked for a file that host can never produce (`Runtime`, not `Runtime.exe`) and told
// the reader to run a macOS shell script. All four now come out of one description
// (Editor/Packaging/PackageTarget.hpp), which is the same description the Build Settings panel shows.
//
// These two tests are what makes that a fact rather than an intention, and they check the produced
// ARTEFACTS rather than the constants: Desert/Tests/Editor/BuildSettingsConsumers already pins the
// table's own relations, and a table that is right while the packager ignores it is precisely the state
// this repair was opened from. PackageGame had no test of any kind before them.

TEST( PackagedContent, PackageGameProducesTheLauncherAndBinaryTheHostDescriptionNames )
{
    EnvironmentGuard guard;

    const Desert::Editor::TargetPlatformInfo& host = Desert::Editor::HostPlatformInfo();

    const fs::path base = fs::temp_directory_path() / "desert_pkg_hosttarget";
    fs::remove_all( base );
    const fs::path proj = base / "proj";

    WriteFile( proj / "GameAssets" / "Scenes" / "level.desce", "scene-body" );
    WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":\"\"}" );

    // The Runtime the packager copies. It looks one directory ABOVE the editor's cwd, which is why the
    // project sits inside `base` rather than being `base`.
    WriteFile( base / "build" / "Bin" / "Release" / host.RuntimeBinary, "not really a binary" );

    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    // The PLAIN layout, because it is the one every host has — the .app branch is macOS-only by
    // construction and asking for it elsewhere is refused (with a log line) rather than obeyed.
    Desert::Editor::PackageOptions options;
    options.OutputDir    = ( base / "out" ).string();
    options.Config       = "Release";
    options.MacAppBundle = false;

    const auto result = Desert::Editor::PackageGame( options );
    ASSERT_TRUE( result.Success ) << result.Message;

    const fs::path root = fs::path( result.PackageDir );
    EXPECT_TRUE( fs::exists( root / host.RuntimeBinary ) )
         << "the packaged player binary is not named what this host names it; a package whose executable "
            "has the wrong name cannot be started on the machine it was made for";
    EXPECT_TRUE( fs::exists( root / host.LauncherName ) )
         << "the package has no " << host.LauncherName
         << " — the launcher was written for a different host's shell";

    // ...and it is that host's shell, not merely that host's file name. The two can disagree, and a
    // `run.bat` full of bash is the failure the file name alone would not catch.
    const auto launcherRead = Common::Utils::FileSystem::ReadFileContent( root / host.LauncherName );
    ASSERT_TRUE( launcherRead.IsSuccess() )
         << "the launcher script did not read back: " << launcherRead.GetError();
    const std::string launcher = launcherRead.GetValue();
    ASSERT_FALSE( launcher.empty() );
    if ( host.Platform == Desert::Editor::TargetPlatform::Windows )
        EXPECT_NE( launcher.find( "@echo off" ), std::string::npos ) << launcher;
    else
        EXPECT_NE( launcher.find( "#!/usr/bin/env bash" ), std::string::npos ) << launcher;

    // The launcher has to actually name the binary beside it, or the package starts nothing.
    EXPECT_NE( launcher.find( host.RuntimeBinary ), std::string::npos ) << launcher;
}

// The refusal, and it is a refusal this suite can produce on any host: no Runtime was built.
//
// What is asserted is that the message names something the reader can RUN. It used to name
// scripts/MacOS/BuildMacOS.sh unconditionally, so on Windows it answered a question about a file that
// could never exist with an instruction that could never help.
TEST( PackagedContent, AMissingRuntimeIsRefusedByNamingThisHostsOwnBuildScript )
{
    EnvironmentGuard guard;

    const Desert::Editor::TargetPlatformInfo& host = Desert::Editor::HostPlatformInfo();

    const fs::path base = fs::temp_directory_path() / "desert_pkg_noruntime";
    fs::remove_all( base );
    const fs::path proj = base / "proj";

    WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":\"\"}" );
    fs::create_directories( proj / "GameAssets" );
    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    Desert::Editor::PackageOptions options;
    options.OutputDir = ( base / "out" ).string();
    options.Config    = "Release"; // nothing was built into base/build/Bin/Release

    const auto result = Desert::Editor::PackageGame( options );
    ASSERT_FALSE( result.Success ) << "a package was produced with no Runtime binary to put in it";
    EXPECT_NE( result.Message.find( host.BuildScript ), std::string::npos )
         << "the refusal does not name a script this host can run: " << result.Message;
    EXPECT_NE( result.Message.find( host.RuntimeBinary ), std::string::npos )
         << "the refusal does not say which file was missing: " << result.Message;
}

// ── A PACKAGE STARTS BY ITSELF (П5) ───────────────────────────────────────────────────────────────
//
// THE DEFECT. The Runtime carried a zero-config branch — mount the archive beside the executable, open
// the descriptor at its root — and NOTHING in this repository produced that descriptor. PackageGame
// wrote `<Name>.deproj` LOOSE beside the archive and generated a launcher script carrying
// `--project <Name>.deproj`, so the branch had never once selected a real game and a player who ran the
// binary directly got "No game to run". Both ends were individually correct: the packager really did
// write a descriptor, the player really did look for one. What was wrong is the RELATION — the name the
// packager wrote and the name the player looked for were different, and they live in two different
// binaries, so nothing anywhere could compare them.
//
// SO THE RELATION IS WHAT THESE TWO TESTS ASSERT, and they assert it by REPLAYING the player's own
// sequence over the packager's own output rather than by checking that a file exists: a file existing
// under a name nobody looks for is precisely the state that shipped. `Runtime/Source/PackagedContent.cpp`
// is compiled into this suite for that reason (see its premake5.lua) — it is the real discovery, not a
// mirror of it.
namespace
{
    // EXACTLY WHAT Runtime/Source/Main.cpp DOES, in its order, with nothing added: find the archives
    // beside the executable, mount them, open the descriptor at the archive root under the one shared
    // name. No arguments are involved anywhere, which is the whole claim.
    struct PlayerStartup
    {
        int         MountExit = Desert::Player::kContentOk;
        std::string MountMessage;
        bool        Opened = false;
    };

    PlayerStartup StartTheGameLikeThePlayerDoes( const fs::path& playerBinary )
    {
        PlayerStartup  out;
        const fs::path baseDir = playerBinary.parent_path();

        const auto content = Desert::Player::MountPackagedContent( baseDir, playerBinary.stem().string() );
        out.MountExit      = content.ExitCode;
        out.MountMessage   = content.Message;
        if ( out.MountExit != Desert::Player::kContentOk )
            return out;

        out.Opened = Desert::Project::ProjectContext::Open(
             ( baseDir / Desert::Project::kPackagedDescriptorName ).string(),
             Desert::Project::ProjectContext::RecordInRecent::No );
        return out;
    }

    // Every loose `.deproj` anywhere under `root`. A package must contain NONE: the descriptor travels
    // inside the archive, and a loose copy beside it is a second source of truth that a zip, a copy or
    // an installer can drop while leaving the archive perfectly intact.
    std::vector<fs::path> LooseDescriptors( const fs::path& root )
    {
        std::vector<fs::path> found;
        std::error_code       ec;
        for ( auto it = fs::recursive_directory_iterator( root, ec ); it != fs::recursive_directory_iterator();
              it.increment( ec ) )
        {
            if ( ec )
                break;
            if ( it->is_regular_file( ec ) && it->path().extension() == ".deproj" )
                found.push_back( it->path() );
        }
        return found;
    }

    // A project whose startup scene sits inside its own (deliberately not "Assets") content root, plus
    // the Runtime binary the packager copies. Returns the project directory.
    fs::path WriteProjectToPackage( const fs::path& base, const char* runtimeBinaryName )
    {
        const fs::path proj = base / "proj";
        WriteFile( proj / "GameAssets" / "Scenes" / "level.desce", "scene-body" );
        WriteFile( proj / "Resources" / "Fonts" / "fake.ttf", "font-body" );
        WriteFile( proj / "T.deproj", "{\"Name\":\"T\",\"AssetsRoot\":\"GameAssets\",\"DefaultScene\":"
                                      "\"GameAssets/Scenes/level.desce\"}" );
        // The packager looks one directory ABOVE the editor's cwd for it.
        WriteFile( base / "build" / "Bin" / "Release" / runtimeBinaryName, "not really a binary" );
        return proj;
    }
} // namespace

TEST( PackagedContent, APackagedGameIsABinaryAndAnArchiveThatStartWithNoArguments )
{
    EnvironmentGuard guard;

    const Desert::Editor::TargetPlatformInfo& host = Desert::Editor::HostPlatformInfo();

    const fs::path base = fs::temp_directory_path() / "desert_pkg_zeroconfig";
    fs::remove_all( base );
    const fs::path proj = WriteProjectToPackage( base, host.RuntimeBinary );

    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    // The PLAIN layout, because it is the one every host has.
    Desert::Editor::PackageOptions options;
    options.OutputDir    = ( base / "out" ).string();
    options.Config       = "Release";
    options.MacAppBundle = false;

    const auto result = Desert::Editor::PackageGame( options );
    ASSERT_TRUE( result.Success ) << result.Message;

    const fs::path root = fs::path( result.PackageDir );
    const fs::path exe  = root / host.RuntimeBinary;

    ASSERT_TRUE( fs::exists( exe ) ) << "no player binary in " << root.string();
    EXPECT_TRUE( fs::exists( root / "Content.dpak" ) )
         << "the archive is not in the same directory as the binary, so the player's one rule for finding "
            "its content - look beside myself - cannot be satisfied without being told where to look";

    const std::vector<fs::path> loose = LooseDescriptors( root );
    EXPECT_TRUE( loose.empty() )
         << "the package carries a loose descriptor (" << ( loose.empty() ? std::string{} : loose[0].string() )
         << "). The descriptor belongs INSIDE the archive: a loose second copy is a file an installer can "
            "drop while the archive survives, and then the game says its content is damaged.";

    const auto launcherRead = Common::Utils::FileSystem::ReadFileContent( root / host.LauncherName );
    ASSERT_TRUE( launcherRead.IsSuccess() ) << launcherRead.GetError();
    EXPECT_EQ( launcherRead.GetValue().find( "--project" ), std::string::npos )
         << "the generated launcher still names a project on the command line. That flag is the DEV door; "
            "a shipped game that needs it is a game that only starts when started the one blessed way:\n"
         << launcherRead.GetValue();

    // ── THE ACCEPTANCE: the player's own sequence, no arguments anywhere in it.
    fs::current_path( root );
    const PlayerStartup started = StartTheGameLikeThePlayerDoes( exe );
    ASSERT_EQ( started.MountExit, Desert::Player::kContentOk ) << started.MountMessage;
    ASSERT_TRUE( started.Opened )
         << "the archive mounted and the player found no game in it - the packager and the discovery "
            "disagree about what a package contains, which is the whole of П5";

    EXPECT_EQ( Desert::Project::ProjectContext::Current().Name, "T" );
    EXPECT_EQ( Desert::Project::ProjectContext::Current().AssetsRoot,
               std::string( Desert::Editor::kPackagedAssetsRoot ) );

    // ...and the scene it boots to is really in the archive. This is the assertion that makes the
    // descriptor's REBASING load-bearing rather than cosmetic: DefaultScene named "GameAssets/..." in
    // the dev tree, the content is packed under "Assets/", and a descriptor that shipped the dev
    // spelling would open, look healthy, and boot to nothing.
    const std::string scene = Desert::Project::ProjectContext::DefaultScenePath();
    ASSERT_FALSE( scene.empty() ) << "the shipped descriptor names no startup scene";
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( scene ), "scene-body" );
}

// The same property under the OTHER layout, and it is one test rather than a macOS-only one because the
// claim is host-independent: whatever this host produces when a bundle is asked for, the archive is in
// the directory the player binary is in. On a bundle host that is Contents/MacOS and Contents/Resources
// is not produced at all — that split is what forced the launcher to pass `--project`, and it is gone
// with it. Everywhere else the request is refused and the plain layout comes back, which satisfies the
// same claim by a different route.
TEST( PackagedContent, TheArchiveSitsBesideThePlayerBinaryInWhicheverLayoutTheHostProduces )
{
    EnvironmentGuard guard;

    const Desert::Editor::TargetPlatformInfo& host = Desert::Editor::HostPlatformInfo();

    const fs::path base = fs::temp_directory_path() / "desert_pkg_bundlelayout";
    fs::remove_all( base );
    const fs::path proj = WriteProjectToPackage( base, host.RuntimeBinary );

    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    Desert::Editor::PackageOptions options;
    options.OutputDir    = ( base / "out" ).string();
    options.Config       = "Release";
    options.MacAppBundle = true;

    const auto result = Desert::Editor::PackageGame( options );
    ASSERT_TRUE( result.Success ) << result.Message;

    const fs::path root = fs::path( result.PackageDir );
    EXPECT_EQ( root.extension() == ".app", host.SupportsAppBundle )
         << "the layout produced and the host description disagree about whether a .app exists here";

    const fs::path exe = host.SupportsAppBundle ? root / "Contents" / "MacOS" / Desert::Editor::kBundlePlayerBinary
                                                : root / host.RuntimeBinary;
    ASSERT_TRUE( fs::exists( exe ) ) << "no player binary at " << exe.string();

    EXPECT_TRUE( fs::exists( exe.parent_path() / "Content.dpak" ) )
         << "the archive is not beside the player binary (" << exe.parent_path().string() << ")";

    if ( host.SupportsAppBundle )
    {
        EXPECT_FALSE( fs::exists( root / "Contents" / "Resources" ) )
             << "Contents/Resources is still produced. Nothing on macOS requires it, and holding the "
                "payload there is exactly what made the launcher hand the descriptor over on the command "
                "line - a second place the player has to be told about.";

        // The plist and the disk must agree about which file macOS starts. They were two independent
        // literals; when they disagree macOS says "damaged application" and nothing else, which is the
        // least diagnosable failure this packager can produce.
        const auto plist = Common::Utils::FileSystem::ReadFileContent( root / "Contents" / "Info.plist" );
        ASSERT_TRUE( plist.IsSuccess() ) << plist.GetError();
        EXPECT_NE( plist.GetValue().find( std::string( "<key>CFBundleExecutable</key><string>" ) +
                                          Desert::Editor::kBundleLauncherName + "</string>" ),
                   std::string::npos )
             << "Info.plist does not declare " << Desert::Editor::kBundleLauncherName
             << " as the bundle executable";
        EXPECT_TRUE( fs::exists( root / "Contents" / "MacOS" / Desert::Editor::kBundleLauncherName ) )
             << "the file Info.plist names as the bundle executable is not in Contents/MacOS";
    }

    fs::current_path( exe.parent_path() );
    const PlayerStartup started = StartTheGameLikeThePlayerDoes( exe );
    ASSERT_EQ( started.MountExit, Desert::Player::kContentOk ) << started.MountMessage;
    EXPECT_TRUE( started.Opened ) << "the bundled game does not start from its own directory";
}

// ── A RELEASE RECORDS WHAT IT HANDED OUT, OR IT CAN NEVER BE UPDATED (П7) ─────────────────────────
//
// THE DEFECT, and it is П5's shape one step further down the same pipeline: a mechanism with a reader
// and no writer. `Runtime/Source/PackagedContent.cpp` mounts every `Patch*.dpak` it finds beside the
// base archive — the consumer has been there since П3 — while `PackageGame()` wrote no manifest at all,
// so `PakTool patch` had no "before" side for any archive this repository can actually ship.
//
// WHY IT COULD NOT BE ADDED LATER, which is what makes it a delivery defect rather than a missing
// feature: a manifest is a record of an archive's own bytes, so it can only be taken while that archive
// exists. A release packaged without one cannot be patched EVER — not by rebuilding, because the
// rebuild is a different archive, and not from the shipped folder, because the folder is the thing the
// patch has to be diffed against, not a description of it.
//
// THESE TESTS HOLD BOTH ENDS AT ONCE, which is possible here and nowhere else: this suite compiles the
// packager AND the player's own discovery (see premake5.lua), and `BuildPatchPak` is the same function
// `PakTool patch` runs. So the whole cycle is one process — package, record, diff, patch, mount, read —
// with no mirror of any step anywhere in it.
namespace
{
    // One release of the currently open project, in the plain layout every host has.
    Desert::Editor::PackageResult PackageInto( const fs::path& outDir )
    {
        Desert::Editor::PackageOptions options;
        options.OutputDir    = outDir.string();
        options.Config       = "Release";
        options.MacAppBundle = false;
        return Desert::Editor::PackageGame( options );
    }
} // namespace

TEST( PackagedContent, APackagedReleaseRecordsAManifestOfTheArchiveItActuallyShipped )
{
    EnvironmentGuard guard;

    const Desert::Editor::TargetPlatformInfo& host = Desert::Editor::HostPlatformInfo();

    const fs::path base = fs::temp_directory_path() / "desert_pkg_manifest";
    fs::remove_all( base );
    const fs::path proj = WriteProjectToPackage( base, host.RuntimeBinary );

    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    Desert::Editor::PackageOptions options;
    options.OutputDir    = ( base / "out" ).string();
    options.Config       = "Release";
    options.MacAppBundle = false;

    const auto result = Desert::Editor::PackageGame( options );
    ASSERT_TRUE( result.Success ) << result.Message;

    // The path comes back as a FIELD. A release script has to be able to pick the file up; a sentence
    // inside `Message` is not something anything can act on (I12's lesson, in the direction it did not
    // reach).
    ASSERT_FALSE( result.ManifestPath.empty() )
         << "the package reports no manifest, so nothing can find the baseline the next update needs";
    const fs::path manifestFile = result.ManifestPath;
    ASSERT_TRUE( fs::exists( manifestFile ) ) << manifestFile.string() << " was reported and not written";

    const fs::path root = fs::path( result.PackageDir );

    // OUTSIDE the package. The product is a binary and one archive (П5); a publisher's record inside the
    // folder a player copies around is one more file an installer can lose and one more thing that reads
    // as content.
    // Compared as PATH COMPONENTS, not as a string prefix: `out/T.manifest` starts with `out/T`
    // character for character while sitting entirely outside it, and a string test here would have
    // reported the file inside the package when it is beside it.
    const fs::path relToPackage = manifestFile.lexically_normal().lexically_relative( root.lexically_normal() );
    EXPECT_TRUE( !relToPackage.empty() && *relToPackage.begin() == ".." )
         << "the release manifest was written INSIDE the package (" << manifestFile.string()
         << "). It is the publisher's record, not the player's: the package is still a binary and one "
            "archive.";

    // ...and it describes THE ARCHIVE, not the trees that went into it. A manifest of the sources would
    // describe a release that does not exist — different by the raw-mesh filter, by the cook, and by the
    // descriptor the packager puts in — and the first symptom would be a patch re-shipping files nobody
    // touched.
    const auto text = Common::Utils::FileSystem::ReadFileContent( manifestFile );
    ASSERT_TRUE( text.IsSuccess() ) << text.GetError();
    auto parsed = Common::Utils::ContentManifest::Parse( text.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << "the recorded manifest does not parse: " << parsed.GetError();

    const Common::Utils::PakReader packed( root / "Content.dpak" );
    ASSERT_TRUE( packed.IsOpen() ) << packed.OpenError();
    const auto fromArchive = Common::Utils::ContentManifest::FromPak( packed );

    EXPECT_TRUE( Common::Utils::CompareManifests( parsed.GetValue(), fromArchive ).Empty() )
         << "the recorded manifest and the archive beside it disagree — " << parsed.GetValue().Count()
         << " entries recorded against " << fromArchive.Count()
         << " in the pak. A baseline that does not describe the shipped archive produces a patch that "
            "does not apply to it.";
    EXPECT_GT( fromArchive.Count(), 0u ) << "the archive is empty, so the comparison above proved nothing";

    // The descriptor is content of the archive and therefore of the manifest: a patch that changed the
    // startup scene and could not carry the new descriptor would be a patch that cannot move a release.
    EXPECT_NE( parsed.GetValue().Find( Desert::Project::kPackagedDescriptorName ), nullptr )
         << "the manifest does not record the descriptor, so no patch can ever replace it";
}

TEST( PackagedContent, AnUpdateBuiltAgainstTheRecordedManifestReachesThePlayerAsNewBytes )
{
    EnvironmentGuard guard;

    const Desert::Editor::TargetPlatformInfo& host = Desert::Editor::HostPlatformInfo();

    const fs::path base = fs::temp_directory_path() / "desert_pkg_patchcycle";
    fs::remove_all( base );
    const fs::path proj = WriteProjectToPackage( base, host.RuntimeBinary );

    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    // ---- release 1: what the player installs, and the record of it the publisher keeps.
    Desert::Editor::PackageOptions v1;
    v1.OutputDir    = ( base / "out1" ).string();
    v1.Config       = "Release";
    v1.MacAppBundle = false;

    const auto first = Desert::Editor::PackageGame( v1 );
    ASSERT_TRUE( first.Success ) << first.Message;
    const fs::path installed = fs::path( first.PackageDir );
    const fs::path baseline  = first.ManifestPath;
    ASSERT_TRUE( fs::exists( baseline ) ) << "release 1 recorded no baseline";

    // ---- release 2: one scene edited, packaged into a directory of its own. The publisher has the new
    // archive and the OLD MANIFEST, and nothing else — which is the whole economy of П3: no old archive
    // is kept anywhere.
    WriteFile( proj / "GameAssets" / "Scenes" / "level.desce", "scene-body-v2" );
    fs::current_path( proj );
    const auto next = PackageInto( base / "out2" );
    ASSERT_TRUE( next.Success ) << next.Message;
    const fs::path second = next.PackageDir;

    // ---- the update, built by the same function `PakTool patch` runs. It is dropped into the INSTALLED
    // release's directory under the name the player's own discovery looks for.
    const auto built =
         Common::Utils::BuildPatchPak( baseline, second / "Content.dpak", installed / "Patch001.dpak" );
    ASSERT_TRUE( built.IsSuccess() ) << built.GetError();
    ASSERT_TRUE( built.GetValue().Written )
         << "two releases with a changed scene produced no patch at all — the baseline is not describing "
            "release 1";

    const std::string sceneKey = std::string( Desert::Editor::kPackagedAssetsRoot ) + "/Scenes/level.desce";
    EXPECT_NE( std::find( built.GetValue().Diff.Changed.begin(), built.GetValue().Diff.Changed.end(), sceneKey ),
               built.GetValue().Diff.Changed.end() )
         << "the patch does not carry the one file that changed (" << sceneKey << ")";

    // ---- the acceptance: the INSTALLED release, started the way a player starts it, reads the new bytes.
    const fs::path exe = installed / host.RuntimeBinary;
    fs::current_path( installed );
    const PlayerStartup started = StartTheGameLikeThePlayerDoes( exe );
    ASSERT_EQ( started.MountExit, Desert::Player::kContentOk ) << started.MountMessage;
    ASSERT_TRUE( started.Opened ) << "the patched installation no longer contains a game";

    const std::string scene = Desert::Project::ProjectContext::DefaultScenePath();
    ASSERT_FALSE( scene.empty() );
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( scene ), "scene-body-v2" );
}

// THE NEGATIVE CONTROL, and it is the half that decides whether any of the above means anything: a
// patch built with no baseline must REFUSE. The alternative is not a small error — an absent manifest
// read as "the previous release was empty" produces a patch that is a whole copy of the new release and
// reports success, which is DC §1.4 at the step where a release is published: a failed result wearing
// the shape of a good one. Quieter than a refusal, and far more expensive.
TEST( PackagedContent, APatchWithNoBaselineIsRefusedByNameAndWritesNothing )
{
    EnvironmentGuard guard;

    const Desert::Editor::TargetPlatformInfo& host = Desert::Editor::HostPlatformInfo();

    const fs::path base = fs::temp_directory_path() / "desert_pkg_nobaseline";
    fs::remove_all( base );
    const fs::path proj = WriteProjectToPackage( base, host.RuntimeBinary );

    SetEnv( "HOME", base.string() );
    fs::current_path( proj );
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( proj / "T.deproj" ).string() ) );

    const auto shipped = PackageInto( base / "out" );
    ASSERT_TRUE( shipped.Success ) << shipped.Message;
    const fs::path release = shipped.PackageDir;

    const fs::path missing = base / "never_recorded.manifest";
    const fs::path out     = base / "Patch001.dpak";

    const auto refused = Common::Utils::BuildPatchPak( missing, release / "Content.dpak", out );
    ASSERT_FALSE( refused.IsSuccess() )
         << "a patch was built with no baseline. Whatever it contains, it is not an update: with no "
            "\"before\" side every entry reads as added, so this is the whole release wearing a patch's "
            "name.";
    EXPECT_NE( refused.GetError().find( missing.filename().string() ), std::string::npos )
         << "the refusal does not name the file that is missing: " << refused.GetError();
    EXPECT_FALSE( fs::exists( out ) ) << "the refusal still left an archive at " << out.string();

    // A file that exists and is not a manifest is the same refusal, not a silently empty base — the
    // truncated-download case, which otherwise reads as "the previous release contained nothing".
    const fs::path garbage = base / "garbage.manifest";
    WriteFile( garbage, "this is not a manifest\n" );
    const auto rejected = Common::Utils::BuildPatchPak( garbage, release / "Content.dpak", out );
    EXPECT_FALSE( rejected.IsSuccess() ) << "a manifest that does not parse was accepted as a baseline";
    EXPECT_FALSE( fs::exists( out ) ) << "the refusal still left an archive at " << out.string();

    // ...and the OTHER outcome that is not a failure: nothing changed. It comes back SUCCESSFUL with
    // `Written == false` and no file, so "there is nothing to ship" and "the patch is at this path" are
    // two answers rather than one answer and an empty file.
    const auto unchanged = Common::Utils::BuildPatchPak( shipped.ManifestPath, release / "Content.dpak", out );
    ASSERT_TRUE( unchanged.IsSuccess() ) << unchanged.GetError();
    EXPECT_FALSE( unchanged.GetValue().Written )
         << "a release patched against its own baseline produced an archive; an empty patch is a file a "
            "publisher can ship believing it fixes something";
    EXPECT_FALSE( fs::exists( out ) );
}

// The rebasing rule on its own, without building anything — the half of the descriptor that a package
// cannot be right without and that costs a whole cook to reach through PackageGame.
TEST( PackagedContent, TheShippedDescriptorRebasesTheStartupSceneAndKeepsEverythingElse )
{
    Common::Project::ProjectFile dev;
    dev.Name          = "T";
    dev.AssetsRoot    = "GameAssets";
    dev.DefaultScene  = "GameAssets/Scenes/level.desce";
    dev.Description   = "a sentence the launcher shows";
    dev.EngineVersion = "0.1.2";

    const Common::Project::ProjectFile shipped = Desert::Editor::PackagedDescriptor( dev );

    EXPECT_EQ( shipped.AssetsRoot, std::string( Desert::Editor::kPackagedAssetsRoot ) );
    EXPECT_EQ( shipped.DefaultScene, "Assets/Scenes/level.desce" );

    // Everything that is not about WHERE the content sits travels unchanged. This used to be rebuilt
    // from three fields, so Description and EngineVersion were dropped by the act of packaging - a
    // package described less of the product than the project did.
    EXPECT_EQ( shipped.Name, dev.Name );
    EXPECT_EQ( shipped.Description, dev.Description );
    EXPECT_EQ( shipped.EngineVersion, dev.EngineVersion );

    // A startup scene OUTSIDE the assets root is not rewritten - there is nothing to rebase it onto,
    // and inventing a path would be the silent substitution §1.4 forbids.
    Common::Project::ProjectFile elsewhere = dev;
    elsewhere.DefaultScene                 = "Somewhere/else.desce";
    EXPECT_EQ( Desert::Editor::PackagedDescriptor( elsewhere ).DefaultScene, "Somewhere/else.desce" );

    // No startup scene stays no startup scene rather than becoming the assets root itself.
    Common::Project::ProjectFile none = dev;
    none.DefaultScene                 = "";
    EXPECT_EQ( Desert::Editor::PackagedDescriptor( none ).DefaultScene, "" );
}

// ── EVERY DECLARED ROOT SHIPS, OR SAYS OUT LOUD WHY IT IS NOT CONTENT ─────────────────────────────
//
// WHY THE CENSUS AT THE TOP OF THIS FILE WAS NOT ENOUGH, stated as what it missed. It walks the roots
// ONE function lists — `ServiceScanRoots` — which is why fonts and icons are safe and why nothing in
// this repository ever noticed that `Resources/Scripts/` existed, held every Lua example the editor
// offered, and was in NO tree the packager builds an archive from. Six scripts, three committed scenes
// naming one, and a packaged game that would have loaded none of them: measured by I8, found by hand,
// caught by no check. A root only enters that census by being scanned by a service; a root that some
// PANEL scans, or that a component field names, enters nothing.
//
// So the relation is turned the other way round and anchored at the DECLARATION instead. Constants.hpp
// is where a root comes into existence, so that is the list that cannot drift: every path constant it
// declares must either be covered by a tree in PackagedContentTrees() — itself or an ancestor of it —
// or carry a written reason why it is not a thing a game contains.
//
// THE ROW THAT DOES THE WORK IS RESOURCE_PATH'S. The engine tree is NOT shipped wholesale; three named
// subtrees below it are. So `Resources/Videos/` added tomorrow, scanned by whoever adds it, has exactly
// two ways past this suite: become a packed tree, or say in one sentence why a game does not need it.
// Neither is something you do by accident, which is the whole point — the previous answer was "nothing
// happens, and you find out when somebody packages the game".
namespace
{
    enum class RootVerdict
    {
        Packaged,   ///< a packed tree, or inside one
        NotContent, ///< deliberately not in the archive, for the stated reason
    };

    struct DeclaredRoot
    {
        const char*     Name; // exactly as Constants.hpp spells it
        const fs::path* Live; // the live constant, so a remap is followed rather than re-typed
        RootVerdict     What;
        const char*     Reason; // NotContent rows only; empty for the others
    };

    // Index over Constants.hpp's declarations. The completeness of THIS table is not trusted — the
    // first test below derives the real set from the header and refuses anything missing.
    const std::vector<DeclaredRoot>& DeclaredRoots()
    {
        namespace P                                  = Common::Constants::Path;
        static const std::vector<DeclaredRoot> roots = {
             // --- engine resources: never remapped, and only these three travel ---
             { "RESOURCE_PATH", &P::RESOURCE_PATH, RootVerdict::NotContent,
               "the engine tree's ROOT, and it is not shipped wholesale - only the three named subtrees "
               "below it are. Anything new placed under it is invisible to the packager until it becomes "
               "a tree of its own here AND in PackagedContentTrees(); Resources/Scripts/ was exactly that "
               "and shipped in nothing for as long as it existed." },
             { "SHADERDIR_PATH", &P::SHADERDIR_PATH, RootVerdict::Packaged, "" },
             { "FONTS_PATH", &P::FONTS_PATH, RootVerdict::Packaged, "" },
             { "ICONS_PATH", &P::ICONS_PATH, RootVerdict::Packaged, "" },

             // --- project content: every row is derived from the assets or cooked root, and both of
             //     those are packed trees, so the whole census travels by construction ---
             { "ASSETS_PATH", &P::ASSETS_PATH, RootVerdict::Packaged, "" },
             { "MESH_PATH", &P::MESH_PATH, RootVerdict::Packaged, "" },
             { "MATERIAL_PATH", &P::MATERIAL_PATH, RootVerdict::Packaged, "" },
             { "TEXTUREDIR_PATH", &P::TEXTUREDIR_PATH, RootVerdict::Packaged, "" },
             { "SKYBOX_PATH", &P::SKYBOX_PATH, RootVerdict::Packaged, "" },
             { "SCENE_PATH", &P::SCENE_PATH, RootVerdict::Packaged, "" },
             { "PREFAB_PATH", &P::PREFAB_PATH, RootVerdict::Packaged, "" },
             { "SCRIPT_PATH", &P::SCRIPT_PATH, RootVerdict::Packaged, "" },
             { "COLLECTIONS_PATH", &P::COLLECTIONS_PATH, RootVerdict::Packaged, "" },
             { "LOCALIZATION_PATH", &P::LOCALIZATION_PATH, RootVerdict::Packaged, "" },
             { "CLOUD_NOISE_PATH", &P::CLOUD_NOISE_PATH, RootVerdict::Packaged, "" },
             { "CLOUD_TYPE_PATH", &P::CLOUD_TYPE_PATH, RootVerdict::Packaged, "" },
             { "CLOUD_VOLUME_PATH", &P::CLOUD_VOLUME_PATH, RootVerdict::Packaged, "" },
             { "CLOUD_LAYOUT_PATH", &P::CLOUD_LAYOUT_PATH, RootVerdict::Packaged, "" },
             { "UI_THEME_PATH", &P::UI_THEME_PATH, RootVerdict::Packaged, "" },
             { "COOKED_PATH", &P::COOKED_PATH, RootVerdict::Packaged, "" },
             { "MESH_PATH_COOKED", &P::MESH_PATH_COOKED, RootVerdict::Packaged, "" },
             { "TEXTURE_PATH_COOKED", &P::TEXTURE_PATH_COOKED, RootVerdict::Packaged, "" },
        };
        return roots;
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 8; ++up )
        {
            std::ifstream probe( prefix + "Desert/Common/Source/Common/Core/Constants.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // Every path constant Constants.hpp DECLARES, by name. A declaration is
    // `inline const std::filesystem::path[&] <NAME> =` — the `=` is what separates a declaration from
    // `Dir( ContentDir d )`, which has the same prefix and is a function.
    std::vector<std::string> DeclaredRootNamesInTheHeader( const std::string& source )
    {
        static const std::string kPrefix = "inline const std::filesystem::path";

        std::vector<std::string> names;
        for ( std::size_t at = source.find( kPrefix ); at != std::string::npos;
              at             = source.find( kPrefix, at + 1 ) )
        {
            std::size_t i = at + kPrefix.size();
            while ( i < source.size() && ( source[i] == '&' || source[i] == ' ' ) )
                ++i;
            const std::size_t nameStart = i;
            while ( i < source.size() &&
                    ( std::isalnum( static_cast<unsigned char>( source[i] ) ) != 0 || source[i] == '_' ) )
                ++i;
            if ( i == nameStart )
                continue;
            const std::string name = source.substr( nameStart, i - nameStart );

            std::size_t after = i;
            while ( after < source.size() && source[after] == ' ' )
                ++after;
            if ( after < source.size() && source[after] == '=' )
                names.push_back( name );
        }
        return names;
    }

    // Is `path` the packed tree `tree`, or inside it? Component-wise, because these paths carry a
    // trailing separator and a string prefix test would also match "Resources/AssetsOther/".
    bool IsAtOrInside( const fs::path& path, const fs::path& tree )
    {
        const fs::path rel = path.lexically_normal().lexically_relative( tree.lexically_normal() );
        if ( rel.empty() )
            return false;
        return *rel.begin() != "..";
    }
} // namespace

// 1. NO UNDECLARED ROW AND NO UNREGISTERED ROOT. The header is the source of truth in both directions:
//    a constant added there without a row here fails, and a row here whose constant is gone fails too.
TEST( PackagedContent, EveryRootConstantTheHeaderDeclaresIsInThePackagingRegister )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository root was not found from the working directory";

    std::ifstream in( root + "Desert/Common/Source/Common/Core/Constants.hpp" );
    ASSERT_TRUE( in.is_open() );
    std::ostringstream buffer;
    buffer << in.rdbuf();

    const std::vector<std::string> declared = DeclaredRootNamesInTheHeader( buffer.str() );
    ASSERT_FALSE( declared.empty() ) << "no path constant was found in Constants.hpp, which cannot be "
                                        "true - the parser is broken, not the header";

    std::set<std::string> registered;
    for ( const DeclaredRoot& row : DeclaredRoots() )
        registered.insert( row.Name );

    for ( const std::string& name : declared )
    {
        EXPECT_EQ( registered.count( name ), 1u )
             << name
             << " is a content root the engine can read from and nothing says whether a PACKAGED GAME "
                "gets it. Add a row: Packaged (and a tree in PackagedContentTrees() that covers it), or "
                "NotContent with the reason a game does not need it.";
    }

    const std::set<std::string> present( declared.begin(), declared.end() );
    for ( const DeclaredRoot& row : DeclaredRoots() )
    {
        EXPECT_EQ( present.count( row.Name ), 1u )
             << row.Name << " is registered here but Constants.hpp no longer declares it - a stale row.";
    }
}

// 2. THE VERDICT IS TRUE, not merely written. Checked under the PACKAGED remap, because that is the
//    only world in which the answer matters and the dev-time spellings would flatter it.
TEST( PackagedContent, EveryRootCalledContentIsCoveredByATreeThePackagerPacks )
{
    EnvironmentGuard guard;

    const fs::path pkg = fs::temp_directory_path() / "desert_pkg_rootcensus";
    Common::Constants::Path::SetProjectRoot( pkg, Desert::Editor::kPackagedAssetsRoot );

    const auto trees = Desert::Editor::PackagedContentTrees();

    for ( const DeclaredRoot& row : DeclaredRoots() )
    {
        bool covered = false;
        for ( const auto& tree : trees )
            covered = covered || IsAtOrInside( *row.Live, *tree.Tree );

        if ( row.What == RootVerdict::Packaged )
        {
            EXPECT_TRUE( covered ) << row.Name << " (" << row.Live->string()
                                   << ") is registered as content that ships, and no tree in "
                                      "PackagedContentTrees() contains it. Everything under it is "
                                      "missing from the archive, and the failure lands on a player.";
        }
        else
        {
            EXPECT_STRNE( row.Reason, "" ) << row.Name << " is excluded from the package with no reason given.";
        }
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
