// EVERY `Preload*` THE ASSET LAYER DECLARES IS CALLED BY BOTH LAYERS THAT START THE ENGINE.
//
// WHY THIS SUITE EXISTS, AND IT IS THE STRONGEST ARGUMENT IN IT. `AssetPreloader::PreloadCloudLayouts`
// was written with the painted layout, scanned `Clouds/Layouts`, registered every `.dclayout` with the
// service, and was CALLED BY NOBODY. Both layers list the preloads themselves, one at a time, in an
// order they need to control; the new one was added to a `PreloadAllAssets()` helper that had had no
// caller since 2023. So the whole feature was dead end to end from the day it shipped: every scene
// binding a painting logged "referenced but not registered" and rendered its sky procedurally, and every
// test of the format, the bake and the panel passed, because not one of them starts a layer.
//
// That is the defect shape this project keeps paying for — both ends correct, the link between them
// missing — and it is exactly the kind no unit test of either end can see. The relation is between a
// class's DECLARATIONS and two call sites in files no header includes, so it is asserted by reading the
// sources, the way TextureSourceFormatCensus reads them next door.
//
// WHAT WOULD MAKE THIS RED, and each is a real mistake: adding a `Preload*` and wiring it into one layer
// only (a packaged game silently missing that content); adding one and wiring it into neither; deleting
// a call from a layer while the method stays.
//
// ---------------------------------------------------------------------------------------------------
// THE SECOND SUBJECT IN THIS FILE, AND IT IS THE SAME QUESTION ONE CONTENT KIND LATER: who fills the
// animation library, and when. It is here rather than in a suite of its own on purpose — a second suite
// asking "is the content index reached from both hosts" would be a second answer to one question, and the
// two would drift.
//
// What was broken, measured on 2026-09-09. `Runtime/Source/RuntimeLayer.cpp` created an AnimationLibrary
// and handed it straight to `AnimationECSSystem` with NOT ONE `Register` call in between — the word
// `AnimationLibrary` appeared exactly twice in the whole file. So the library of a PACKAGED GAME was
// empty and every skinned character stood in its bind pose. `Editor/Source/EditorLayer.cpp` had its own
// copy of the fill loop, which is why no editor session could reproduce it — and that copy was ALSO
// wrong, in the other direction: it ran in `OnAttach`, several startup stages before the scan that finds
// `.anim` files on disk, so with three clips in `Cooked/Meshes` the editor reported "4 clip(s) known",
// exactly the four compiled-in procedural ones.
//
// Both halves are one thing — "who and when fills the library" — and the fix is one point,
// `Animation::PopulateLibrary`, called from the tail of the scan that finds the clips. The checks below
// pin all three properties that make that fix hold: the point exists inside the scan, the scan's own
// count is what it is given (so the ordering cannot be reversed and still compile), and neither host has
// grown a copy of the loop again.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // The two files that start the engine. There is no third: the editor's startup stage list and the
    // runtime's straight-line sequence are the only places a preload is ever asked for.
    constexpr const char* kPreloaderHeader = "Desert/Desert/Source/Engine/Assets/AssetPreloader.hpp";
    constexpr const char* kPreloaderSource = "Desert/Desert/Source/Engine/Assets/AssetPreloader.cpp";
    constexpr const char* kLayers[] = { "Editor/Source/EditorLayer.cpp", "Runtime/Source/RuntimeLayer.cpp" };

    // Spellings by which a HOST would be filling the animation library itself. Each is a line that used to
    // exist in EditorLayer.cpp and must not come back in either layer: the population point is shared, and
    // a host that re-grows its own copy re-creates both halves of the defect at once — the other host
    // silently has none, and this one runs at whatever moment its own startup happens to reach.
    constexpr const char* kHostFillSpellings[] = {
         "m_AnimationLibrary->Register(",
         "m_AnimationLibrary->Clear(",
         "ProceduralCharacterAnimations::RegisterClips",
         "PopulateLibrary",
    };

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + kPreloaderHeader );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Declarations, not definitions: the header is the census, and a method declared and never defined
    // would not link anyway. Comment lines are skipped so that a method NAMED in a comment — this
    // header carries one, explaining the deleted `PreloadAllAssets` — is not read as a declaration.
    std::vector<std::string> DeclaredPreloads( const std::string& header )
    {
        std::vector<std::string> names;
        const std::regex         pattern( R"(^\s*void\s+(Preload[A-Za-z0-9_]*)\s*\()" );
        std::istringstream       lines( header );
        std::string              line;
        while ( std::getline( lines, line ) )
        {
            const size_t first = line.find_first_not_of( " \t" );
            if ( first != std::string::npos && line.compare( first, 2, "//" ) == 0 )
                continue;

            std::smatch match;
            if ( std::regex_search( line, match, pattern ) )
                names.push_back( match[1].str() );
        }
        return names;
    }

    /**
     * @brief The source with its comments blanked out, so a check about CODE cannot be satisfied — or
     *        broken — by prose.
     *
     * BOTH DIRECTIONS HAVE BITTEN. This file's own `DeclaredPreloads` skips comment lines because the
     * preloader header NAMES a deleted method in a comment; and the checks below would fail on the
     * comment `EditorLayer.cpp` now carries explaining that the fill loop moved, which mentions
     * `PopulateLibrary` by name. Blanking rather than deleting keeps byte offsets intact, which the
     * ordering check depends on.
     *
     * String and character literals are tracked because `//` inside one is not a comment — the layers
     * contain URL-shaped strings, and a naive stripper would eat the rest of those lines.
     */
    std::string WithoutComments( const std::string& source )
    {
        std::string out = source;
        enum class In
        {
            Code,
            LineComment,
            BlockComment,
            String,
            Char
        } state = In::Code;

        for ( size_t i = 0; i < out.size(); ++i )
        {
            const char c    = out[i];
            const char next = ( i + 1 < out.size() ) ? out[i + 1] : '\0';

            switch ( state )
            {
                case In::Code:
                    if ( c == '/' && next == '/' )
                    {
                        state    = In::LineComment;
                        out[i]   = ' ';
                        out[++i] = ' ';
                    }
                    else if ( c == '/' && next == '*' )
                    {
                        state    = In::BlockComment;
                        out[i]   = ' ';
                        out[++i] = ' ';
                    }
                    else if ( c == '"' )
                        state = In::String;
                    else if ( c == '\'' )
                        state = In::Char;
                    break;

                case In::LineComment:
                    if ( c == '\n' )
                        state = In::Code;
                    else
                        out[i] = ' ';
                    break;

                case In::BlockComment:
                    if ( c == '*' && next == '/' )
                    {
                        state    = In::Code;
                        out[i]   = ' ';
                        out[++i] = ' ';
                    }
                    else if ( c != '\n' )
                        out[i] = ' ';
                    break;

                case In::String:
                    if ( c == '\\' )
                        ++i;
                    else if ( c == '"' )
                        state = In::Code;
                    break;

                case In::Char:
                    if ( c == '\\' )
                        ++i;
                    else if ( c == '\'' )
                        state = In::Code;
                    break;
            }
        }
        return out;
    }

    /// The body of @p function in @p source, comments already blanked: from its name to the matching
    /// closing brace. Empty when the function is not there, which the callers assert on.
    std::string BodyOf( const std::string& source, const std::string& function )
    {
        const size_t at = source.find( function );
        if ( at == std::string::npos )
            return {};
        const size_t open = source.find( '{', at );
        if ( open == std::string::npos )
            return {};

        int depth = 0;
        for ( size_t i = open; i < source.size(); ++i )
        {
            if ( source[i] == '{' )
                ++depth;
            else if ( source[i] == '}' && --depth == 0 )
                return source.substr( open, i - open + 1 );
        }
        return {};
    }

    /// Every place a skybox ASSET HANDLE is bound to something that will draw with it. Each one has to
    /// be able to build the environment itself, because the boot no longer builds all of them — see
    /// `AssetPreloadCensus.TheSkyboxStageScansWithoutBaking`. The spellings are the actual call, not a
    /// bare `Register(`: a renamed local should make this red and be named here again, rather than be
    /// silently satisfied by some other Register in the file.
    struct SkyboxBindSite
    {
        const char* File;
        const char* Spelling;
    };
    constexpr SkyboxBindSite kSkyboxBindSites[] = {
         { "Desert/Desert/Source/Engine/Runtime/Services/AssetServiceRegistration.cpp",
           "service->Register( skybox )" },
         { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/SkyboxComponent.cpp", "svc.Register(" },
         { "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp", "svc->Register(" },
    };
} // namespace

TEST( AssetPreloadCensus, TheHeaderStillDeclaresPreloadsAtAll )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    const std::vector<std::string> declared = DeclaredPreloads( ReadFile( root + kPreloaderHeader ) );

    // A census that found nothing would pass the test below for the wrong reason — the failure mode of
    // every source-scanning suite, and the one it has to rule out about itself first.
    EXPECT_GE( declared.size(), 5u ) << "the scan found " << declared.size() << " Preload* declarations in "
                                     << kPreloaderHeader
                                     << ", which means the parse stopped matching rather than that the "
                                        "asset layer shrank";
}

TEST( AssetPreloadCensus, EveryPreloadIsCalledByBothLayers )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::vector<std::string> declared = DeclaredPreloads( ReadFile( root + kPreloaderHeader ) );
    ASSERT_FALSE( declared.empty() );

    for ( const char* layer : kLayers )
    {
        const std::string source = ReadFile( root + layer );
        ASSERT_FALSE( source.empty() ) << "could not read " << layer;

        for ( const std::string& name : declared )
            // A CALL, arguments or not: SPL2 passes the splash's progress rows (`SplashItems()`), so the
            // census looks for a member call `->Name(` / `.Name(`, which a declaration cannot satisfy.
            EXPECT_TRUE( source.find( "->" + name + "(" ) != std::string::npos ||
                         source.find( "." + name + "(" ) != std::string::npos )
                 << layer << " never calls AssetPreloader::" << name
                 << "(). A preload nothing calls is content that silently never loads: the scenes that "
                    "reference it log one line and render without it, and no test of the asset, the "
                    "format or the panel can see it. Add the call, or delete the method.";
    }
}

// THE POPULATION POINT LIVES INSIDE THE SCAN, which is what makes it reachable from both hosts without
// either host naming it: `AssetPreloadCensus.EveryPreloadIsCalledByBothLayers` above already proves both
// layers call `PreloadCookedAssetsAndMaterials()`, so a fill at its tail runs in both, once, always.
TEST( AssetPreloadCensus, TheAnimationLibraryIsFilledByTheScanThatFindsTheClips )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string source = WithoutComments( ReadFile( root + kPreloaderSource ) );
    ASSERT_FALSE( source.empty() ) << "could not read " << kPreloaderSource;

    EXPECT_NE( source.find( "Animation::PopulateLibrary(" ), std::string::npos )
         << kPreloaderSource
         << " never calls Animation::PopulateLibrary(). The animation library is an index over clip assets "
            "exactly as the texture, mesh and material services are indexes over theirs, and every one of "
            "those is published to HERE, at the tail of the scan that finds them. A library filled anywhere "
            "else is filled by a host — which is how the packaged game shipped with an empty one and the "
            "editor filled its own before the clips had been scanned.";
}

// THE ORDER IS A DATA DEPENDENCY, NOT A LINE NUMBER. This is the check that outlives a refactor: the
// number of `.anim` files exists only after the scan has run, so a fill moved above the scan does not
// compile. What a refactor CAN silently do is drop the parameter and with it the guarantee, and that is
// what reddens here.
TEST( AssetPreloadCensus, ThePopulationIsGivenTheScansOwnCountAndSoCannotPrecedeIt )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string source = WithoutComments( ReadFile( root + kPreloaderSource ) );
    ASSERT_FALSE( source.empty() );

    // The variable the `.anim` scan's result is bound to. `[\s\S]` rather than `.` because the assignment
    // is wrapped across lines by the formatter.
    //
    // `ProcessAssetKind` SINCE T2.4, and the rename is the whole of what changed here: the scan takes
    // its candidates from the cooked asset registry instead of from a directory walk, so the function
    // no longer needs a root or an extension list — but it still RETURNS the count, and that count is
    // still the only thing that can tell "this project has no clips" from "the clips never reached the
    // library".
    std::smatch      scan;
    const std::regex scanPattern( R"((\w+)\s*=\s*[\s\S]{0,40}?ProcessAssetKind<\s*AnimationAsset\s*>)" );
    ASSERT_TRUE( std::regex_search( source, scan, scanPattern ) )
         << "the `.anim` scan in " << kPreloaderSource
         << " no longer assigns its result to anything. That count is the only thing that can tell 'this "
            "project has no clips' from 'the clips never reached the library', and it is what makes the "
            "fill impossible to move above the scan.";
    const std::string counter = scan[1].str();

    const size_t scanAt = static_cast<size_t>( scan.position( 0 ) );
    const size_t fillAt = source.find( "Animation::PopulateLibrary(" );
    ASSERT_NE( fillAt, std::string::npos );
    EXPECT_LT( scanAt, fillAt ) << "the animation library is filled BEFORE the `.anim` scan in "
                                << kPreloaderSource
                                << ". That is the editor's old defect moved into the asset layer: the fill "
                                   "would run against a manager that has not been shown a clip file yet.";

    // The call's own argument list must carry that variable. Bounded by the statement's semicolon rather
    // than by brace matching — the call is one statement.
    const size_t      fillEnd = source.find( ';', fillAt );
    const std::string call    = source.substr( fillAt, fillEnd - fillAt );
    EXPECT_NE( call.find( counter ), std::string::npos )
         << "Animation::PopulateLibrary is no longer given '" << counter
         << "', the count the `.anim` scan produced. Without that argument the ordering is back to being a "
            "convention about line order, and an empty library stops being distinguishable from a project "
            "with no clips in it.";
}

// AND NEITHER HOST FILLS IT ITSELF. The editor's own loop is what hid the runtime's missing one for as
// long as it did, so the absence of a host-side fill is part of the fix rather than a tidiness rule.
TEST( AssetPreloadCensus, NoLayerFillsTheAnimationLibraryItself )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const char* layer : kLayers )
    {
        const std::string source = WithoutComments( ReadFile( root + layer ) );
        ASSERT_FALSE( source.empty() ) << "could not read " << layer;

        // The census has to rule out its own empty answer first: a stripper that blanked the whole file
        // would pass every check below for the wrong reason.
        ASSERT_NE( source.find( "m_AnimationLibrary" ), std::string::npos )
             << layer
             << " no longer mentions m_AnimationLibrary at all, so this check is asserting "
                "nothing. Either the layer stopped owning a library — in which case this suite "
                "needs rewriting — or the comment stripper ate the file.";

        for ( const char* spelling : kHostFillSpellings )
            EXPECT_EQ( source.find( spelling ), std::string::npos )
                 << layer << " contains '" << spelling
                 << "', so this host fills the animation library itself. Both halves of the defect this "
                    "forbids were live at once: the editor had exactly this loop and ran it before the "
                    "`.anim` scan, and the runtime had nothing and shipped T-posing characters. The fill "
                    "belongs to Animation::PopulateLibrary, called from AssetPreloader.";
    }
}

// ---------------------------------------------------------------------------------------------------
// THE THIRD SUBJECT: A STAGE THAT SCANS IS NOT A STAGE THAT BUILDS, and the skybox stage used to be both.
//
// `PreloadSkyboxes` scanned every `.hdr` in the project AND called `SkyboxService::Register` on each one,
// which constructs a MaterialSkybox, which runs the whole radiance/irradiance/prefilter compute chain.
// Measured on this machine, that stage was 320.0 ms of a 352.4 ms staged boot — 90.8 % of it — for one
// 32 KB `.hdr`; deleting the loop took the staged boot to 45.7 ms and the stage itself to 0.1 ms. A
// second, unreferenced 8 MiB `.hdr` added 1119 ms on top of that, so the cost is per FILE. The comment that stood
// over the loop named the benefit it bought ("selecting an HDR skybox in the editor is instant, no per-select
// compute stall"), and the benefit was real; what it did not say is that every boot paid it for every skybox
// nobody asked for, which is the same shape as the three cloud stages above.
//
// WHAT MAKES THE DELETION SAFE IS NOT A NEW MECHANISM — it is that every site which binds a skybox handle
// ALREADY registers on demand, each with its own `WaitDeviceIdle`, and has done since before this
// change. The scene deserialiser does it, the component's picker does it, the material editor's slot
// does it. That is the relation this pair of tests pins, because it is the relation that would rot: a
// future edit that removes one of those three on-demand registrations restores the old defect shape (a
// scene naming a skybox that nothing ever baked draws no sky and says nothing), and the eager preload
// that used to cover for it is gone.
// ---------------------------------------------------------------------------------------------------
TEST( AssetPreloadCensus, TheSkyboxStageScansWithoutBaking )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string source = WithoutComments( ReadFile( root + kPreloaderSource ) );
    ASSERT_FALSE( source.empty() ) << "could not read " << kPreloaderSource;

    const std::string body = BodyOf( source, "void AssetPreloader::PreloadSkyboxes" );
    ASSERT_FALSE( body.empty() ) << "AssetPreloader::PreloadSkyboxes is not where this suite expects it";

    // THE SCAN STAYS, and it is not vestigial: the component's picker lists the project's skyboxes by
    // asking the asset manager for `FindAllByType<SkyboxAsset>()`, which is exactly what this mints.
    EXPECT_NE( body.find( "ProcessAssetKind<SkyboxAsset>" ), std::string::npos )
         << "PreloadSkyboxes no longer scans. The scan is what mints every `.hdr`'s handle, so without it "
            "the Skybox component's picker dropdown is empty and a scene's reference has no handle to "
            "resolve against.";

    EXPECT_EQ( body.find( "GetSkyboxService" ), std::string::npos )
         << "PreloadSkyboxes is building IBL environments at boot again. Measured, that stage was "
            "320.0 ms of a 352.4 ms staged boot, against 45.7 ms without it, and it grows per file; the "
            "three sites that bind a skybox handle all register on demand already (see "
            "AssetPreloadCensus.EverySkyboxBindSiteCanBuildItsOwnEnvironment). If the per-select stall is "
            "the problem being solved, the answer is an asynchronous bake with a Pending state, the way "
            "the cloud kinds answered it — not paying for every file on every boot.";
}

TEST( AssetPreloadCensus, EverySkyboxBindSiteCanBuildItsOwnEnvironment )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const auto& site : kSkyboxBindSites )
    {
        const std::string source = WithoutComments( ReadFile( root + site.File ) );
        ASSERT_FALSE( source.empty() ) << "could not read " << site.File;

        // Rule out the empty answer first: a stripper that blanked the file would satisfy nothing below
        // and pass anyway.
        ASSERT_NE( source.find( "GetSkyboxService" ), std::string::npos )
             << site.File << " no longer reaches the skybox service at all, so this check is asserting nothing.";

        EXPECT_NE( source.find( site.Spelling ), std::string::npos )
             << site.File << " no longer contains '" << site.Spelling
             << "', so it binds a skybox handle without being able to build that skybox's environment. "
                "The boot stopped baking every `.hdr` in the project (see "
                "AssetPreloadCensus.TheSkyboxStageScansWithoutBaking), so this site is now the only "
                "thing standing between a bound handle and a scene with no sky and no log line. If the "
                "call was renamed, name the new spelling in kSkyboxBindSites.";
    }
}

// THE SCENE PARSE RESOLVES A SKYBOX BY TWO SPELLINGS, and each has to build the environment. The census
// above held ComponentRegistry.cpp to "reaches the skybox service somewhere in the file", which the PATH
// branch satisfied while the GUID branch — the only one a SCNE 31 scene takes — returned the scanned
// record's handle and registered nothing: black sky, no ambient, and no log line (RSKY3). So the
// relation is asserted per branch: every `"SkyboxAsset"` arm of both resolvers calls the one helper.
TEST( AssetPreloadCensus, BothSceneSkyboxResolversBuildTheEnvironment )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const char*       file   = "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp";
    const std::string source = WithoutComments( ReadFile( root + file ) );
    ASSERT_FALSE( source.empty() ) << "could not read " << file;

    for ( const char* resolver : { "r.FromPath = ", "r.FromGuid = " } )
    {
        const std::string lambda = BodyOf( source, resolver );
        ASSERT_FALSE( lambda.empty() ) << file << " has no '" << resolver
                                       << "' lambda where this suite expects it";

        const std::string branch = BodyOf( lambda, "type == \"SkyboxAsset\"" );
        ASSERT_FALSE( branch.empty() ) << "the '" << resolver << "' lambda has no \"SkyboxAsset\" branch";

        EXPECT_NE( branch.find( "EnsureSkyboxRegistered(" ), std::string::npos )
             << "the \"SkyboxAsset\" branch of '" << resolver << "' in " << file
             << " returns a skybox handle without registering it. The boot only SCANS skyboxes, so the "
                "SkyboxService stays empty, the SkyboxCommand carries no cube and DeferredLighting logs "
                "'irradiance cube MISSING' — a black sky. Call Runtime::EnsureSkyboxRegistered.";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
