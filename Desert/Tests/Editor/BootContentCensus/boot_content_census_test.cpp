// NO ASSET KIND HAS A BOOT STAGE (AL1-9), AND WHAT A HOST DOES LOAD BEFORE ITS FIRST FRAME IS CALLED BY BOTH.
//
// `AssetPreloader` is deleted. It was a class of boot stages, each of which created a shell for every file of
// a kind; AL1-1..AL1-9 moved every kind onto its content-registry row, created when something names it. What
// is left (`Engine/Assets/BootContent.hpp`) is plan 2.4(a): engine shaders, the animation index, the current
// language's string tables. This census pins the three relations that make the deletion hold:
//   1. no `Preload*` and no `AssetPreloader` in the code of the engine, the editor or the runtime - a stage
//      that came back would be the eager scan this programme removed (`PreloadCloudLayouts` shipped dead for a
//      month because a stage existed that no host called);
//   2. every function BootContent declares `void` is called by BOTH hosts, and BootContent creates no shell
//      other than the engine shaders;
//   3. every place a skybox handle is bound requires it from the registry row, so a bound handle is never a
//      black sky because nothing created the shell.
// Read from the sources, as TextureSourceFormatCensus does: the relations are between files no header joins.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr const char* kBootHeader  = "Desert/Desert/Source/Engine/Assets/BootContent.hpp";
    constexpr const char* kBootSource  = "Desert/Desert/Source/Engine/Assets/BootContent.cpp";
    constexpr const char* kLayers[]    = { "Editor/Source/EditorLayer.cpp", "Runtime/Source/RuntimeLayer.cpp" };
    constexpr const char* kCodeRoots[] = { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source",
                                           "Runtime/Source" };

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::ifstream( prefix + kBootHeader ) )
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

    /**
     * @brief The source with its comments blanked out, so a check about CODE cannot be satisfied - or
     *        broken - by prose. Blanking rather than deleting keeps byte offsets intact.
     *
     * String and character literals are tracked because `//` inside one is not a comment - the layers
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

    // Spellings by which a HOST would fill the animation library itself instead of calling BootContent.
    constexpr const char* kHostFillSpellings[] = {
         "m_AnimationLibrary->Register(",
         "m_AnimationLibrary->Clear(",
         "ProceduralCharacterAnimations::RegisterClips",
         "PopulateLibrary",
    };

    // Every place a skybox handle is bound to something that draws with it.
    constexpr const char* kSkyboxBindSites[] = {
         "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp",
         "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/SkyboxComponent.cpp",
         "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp",
         "Editor/Source/Editor/Panels/SkyboxViewer/SkyboxViewerDocument.cpp",
         "Editor/Source/Editor/Widgets/PreviewEnvironmentUI.cpp",
    };
} // namespace

TEST( BootContentCensus, ThePreloaderIsGone )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    for ( const char* file : { "Desert/Desert/Source/Engine/Assets/AssetPreloader.hpp",
                               "Desert/Desert/Source/Engine/Assets/AssetPreloader.cpp" } )
        EXPECT_FALSE( std::filesystem::exists( root + file ) ) << file << " is back; AL1-9 deleted it.";
}

TEST( BootContentCensus, NoPreloadAnywhereInTheCode )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::regex         preload( R"(\bPreload[A-Z]\w*|\bAssetPreloader\b)" );
    std::size_t              scanned = 0;
    std::vector<std::string> offenders;
    for ( const char* dir : kCodeRoots )
    {
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root + dir ) )
        {
            const auto ext = entry.path().extension();
            if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" && ext != ".h" ) )
                continue;
            ++scanned;
            const std::string code = WithoutComments( ReadFile( entry.path().string() ) );
            std::smatch       match;
            if ( std::regex_search( code, match, preload ) )
                offenders.push_back( entry.path().generic_string() + ": " + match.str() );
        }
    }
    ASSERT_GT( scanned, 500u ) << "the walk read too few sources to be asserting anything";
    for ( const auto& offender : offenders )
        ADD_FAILURE() << offender
                      << " - a boot stage is back. Kinds are created from their content-registry row when "
                         "named; what a host must load before its first frame belongs in BootContent.hpp.";
}

TEST( BootContentCensus, EveryBootFunctionIsCalledByBothHosts )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string        header = WithoutComments( ReadFile( root + kBootHeader ) );
    const std::regex         declaration( R"(\bvoid\s+(\w+)\s*\()" );
    std::vector<std::string> declared;
    for ( auto it = std::sregex_iterator( header.begin(), header.end(), declaration );
          it != std::sregex_iterator(); ++it )
        declared.push_back( ( *it )[1].str() );
    ASSERT_EQ( declared.size(), 3u ) << "BootContent.hpp declares " << declared.size()
                                     << " void functions; the census expects shaders, clips, string tables";

    for ( const char* layer : kLayers )
    {
        const std::string code = WithoutComments( ReadFile( root + layer ) );
        ASSERT_FALSE( code.empty() ) << "could not read " << layer;
        for ( const std::string& name : declared )
            EXPECT_NE( code.find( "Assets::" + name + "(" ), std::string::npos )
                 << layer << " never calls Assets::" << name
                 << "; a boot function one host skips is content that exists in one host only.";
        for ( const char* spelling : kHostFillSpellings )
            EXPECT_EQ( code.find( spelling ), std::string::npos )
                 << layer << " fills the animation library itself (" << spelling
                 << "); call Assets::IndexAnimationClips instead.";
    }
}

TEST( BootContentCensus, TheBootCreatesNoShellButTheEngineShaders )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string code = WithoutComments( ReadFile( root + kBootSource ) );
    ASSERT_FALSE( code.empty() );
    std::size_t created = 0;
    for ( std::size_t at = code.find( "CreateAsset<" ); at != std::string::npos;
          at             = code.find( "CreateAsset<", at + 1 ) )
    {
        ++created;
        EXPECT_EQ( code.compare( at, 24, "CreateAsset<ShaderAsset>" ), 0 )
             << "BootContent.cpp creates something other than an engine shader: " << code.substr( at, 48 );
    }
    EXPECT_EQ( created, 1u );
    for ( const char* token : { "FindAllByType", "CreateFromRegistryRow", "ContentKind::Skybox",
                                "ContentKind::Texture", "ContentKind::Material", "ContentKind::StaticMesh" } )
        EXPECT_EQ( code.find( token ), std::string::npos ) << "BootContent.cpp names " << token;

    // The clip index is given the registry's own clip count, so it cannot run before the rows exist.
    const std::string index = BodyOf( code, "void IndexAnimationClips(" );
    ASSERT_FALSE( index.empty() );
    const std::size_t rows     = index.find( "ContentRegistry::Rows( Common::Content::ContentKind::Animation )" );
    const std::size_t populate = index.find( "PopulateLibrary(" );
    ASSERT_NE( rows, std::string::npos );
    ASSERT_NE( populate, std::string::npos );
    EXPECT_LT( rows, populate );
}

TEST( BootContentCensus, EverySkyboxBindSiteRequiresTheRegistryRow )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    for ( const char* site : kSkyboxBindSites )
    {
        const std::string code = WithoutComments( ReadFile( root + site ) );
        ASSERT_FALSE( code.empty() ) << "could not read " << site;
        EXPECT_NE( code.find( "Runtime::RequireSkybox(" ), std::string::npos )
             << site << " binds a skybox handle without requiring it from the registry row";
        for ( const char* shell : { "FindByHandle<Assets::SkyboxAsset>", "FindByPath<Assets::SkyboxAsset>",
                                    "CreateAsset<Assets::SkyboxAsset>" } )
            EXPECT_EQ( code.find( shell ), std::string::npos )
                 << site << " looks for a skybox shell (" << shell
                 << "); no boot stage creates one any more, so it misses every skybox not yet named.";
    }

    const std::string service = WithoutComments(
         ReadFile( root + "Desert/Desert/Source/Engine/Runtime/Services/Skybox/SkyboxService.cpp" ) );
    EXPECT_NE( BodyOf( service, "SkyboxService::Require(" ).find( "CreateFromRegistryRow<Assets::SkyboxAsset>" ),
               std::string::npos );
    const std::string registry =
         WithoutComments( ReadFile( root + "Desert/Desert/Source/Engine/Runtime/ResourceRegistry.cpp" ) );
    EXPECT_NE( BodyOf( registry, "BindOnDemandAssets(" ).find( "GetSkyboxService()->BindAssetManager" ),
               std::string::npos )
         << "the skybox service is never given the manager its registry shells are created in";
}

// Both spellings of a scene's skybox reference resolve through the same row (RSKY3: the GUID branch once
// returned a scanned record's handle and requested nothing - a black sky with no log line).
TEST( BootContentCensus, BothSceneSkyboxResolversRequireTheSkybox )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const char*       file = "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp";
    const std::string code = WithoutComments( ReadFile( root + file ) );
    ASSERT_FALSE( code.empty() );
    for ( const char* resolver : { "r.FromPath = ", "r.FromGuid = " } )
    {
        const std::string lambda = BodyOf( code, resolver );
        ASSERT_FALSE( lambda.empty() ) << resolver;
        const std::string branch = BodyOf( lambda, "type == \"SkyboxAsset\"" );
        ASSERT_FALSE( branch.empty() ) << resolver << " has no \"SkyboxAsset\" branch";
        EXPECT_NE( branch.find( "RequireSkybox(" ), std::string::npos )
             << "the \"SkyboxAsset\" branch of '" << resolver << "' returns a handle without requiring it";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
