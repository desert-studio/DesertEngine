// THE PROCEDURAL SKY'S RADIANCE CUBE IS A BAKE INTERMEDIATE, AND NOTHING MAY READ IT BACK.
//
// WHAT CHANGED AND WHY IT NEEDS A GATE. `EnvironmentManager::CreateProcedural` builds three cubes from
// the panorama it bakes: radiance, diffuse irradiance and the GGX prefilter. On the procedural path the
// radiance cube has exactly one consumer — `CreatePrefilteredMap`, which convolves it — and then nothing
// at all: `SkyboxRenderer::Render` returns at the fullscreen atmosphere quad before
// `MaterialSkybox::BindInputs` is ever reached, so the sharp cube is never sampled for the backdrop, and
// the ambient/reflection path reads only `IrradianceMap` and `PreFilteredMap`
// (`SceneRenderer.cpp` and `MeshRenderer.cpp`, both gated on those two handles by name). So the cube is
// freed the moment the prefilter has consumed it, beside the panorama that was already freed there for
// the same reason. Measured: 100 663 296 B (96 MiB) per live environment, with six camera points of
// Fog_Showcase byte-identical across the change.
//
// THE .HDR PATH IS NOT THE SAME PATH, and that asymmetry is the thing most likely to be "tidied up" by
// somebody reading only one of the two functions. There the radiance cube has three live runtime
// consumers — the skybox draw (`MaterialSkybox::BindInputs`), the entity preview thumbnail
// (`ScenePropertiesPanel`) and the material editor's cube ball (`MaterialEditorPanel`) — and both
// measured alternatives to it lose: prefilter mip 0 is visibly blockier (max delta 123/255 at zenith on
// real content) and sampling the panorama directly crawls under motion (rms 14.33 vs 10.60 at the zenith
// under a 0.40 deg nudge). So `EnvironmentManager::Create` must keep it.
//
// WHAT A FUTURE MISTAKE LOOKS LIKE, precisely. It is NOT a use-after-free: `ImageService::Resolve` is
// generation-checked, so a handle whose image has been unregistered answers `nullptr` and never somebody
// else's image (ImageService.cpp says so in its own comment, and the third test below pins it). The
// failure is quieter than that — a consumer added to the composed environment
// (`SceneRenderer::GetEnvironment`, which answers the PROCEDURAL environment whenever the sky is
// procedural) would resolve the cube to null and draw nothing, on a path where "this scene has no sky"
// is already a legal state and therefore says nothing in the log. That is why the census below is over
// the FILE SET: a new reader of `RadianceMap` is a new file in the list, whatever it does with it.
//
// NO DEVICE IS NEEDED FOR ANY OF THIS. Two of the three subjects are relations between source files, and
// the third — that a cleared handle resolves to nothing rather than to whatever occupies slot 0 — is
// pure CPU inside `Runtime::ImageService`, which this suite compiles directly.

#include <gtest/gtest.h>

#include <Engine/Runtime/Services/Image/ImageService.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix +
                                       "Desert/Desert/Source/Engine/Graphic/Environment/SceneEnvironment.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::string& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    /// Comments become spaces; newlines survive. Without it every assertion here would be satisfied by
    /// the prose in the file it is reading — and the prose in these files is mostly about this subject.
    std::string StripComments( const std::string& src )
    {
        std::string out;
        out.reserve( src.size() );
        for ( std::size_t i = 0; i < src.size(); )
        {
            if ( src.compare( i, 2, "//" ) == 0 )
            {
                while ( i < src.size() && src[i] != '\n' )
                    ++i;
            }
            else if ( src.compare( i, 2, "/*" ) == 0 )
            {
                i += 2;
                while ( i + 1 < src.size() && src.compare( i, 2, "*/" ) != 0 )
                    out += ( src[i++] == '\n' ) ? '\n' : ' ';
                i += 2;
            }
            else
            {
                out += src[i++];
            }
        }
        return out;
    }

    /// The body of @p function in @p source, comments already stripped: from its name to the matching
    /// closing brace. Empty when the function is not there, which every caller asserts on.
    std::string BodyOf( const std::string& source, const std::string& function )
    {
        const std::size_t at = source.find( function );
        if ( at == std::string::npos )
            return {};
        const std::size_t open = source.find( '{', at );
        if ( open == std::string::npos )
            return {};

        int depth = 0;
        for ( std::size_t i = open; i < source.size(); ++i )
        {
            if ( source[i] == '{' )
                ++depth;
            else if ( source[i] == '}' && --depth == 0 )
                return source.substr( open, i - open + 1 );
        }
        return {};
    }

    /// `find` on an identifier is not enough, and this file learned it the hard way: `radianceHandle`
    /// is a suffix of `diffuseIrradianceHandle`, so a plain search says the freed handle escaped into
    /// the return statement when what it actually saw was the IRRADIANCE handle going out legitimately.
    /// Matches only where neither neighbour could be part of a C++ identifier.
    std::size_t FindIdentifier( const std::string& haystack, const std::string& name, std::size_t from )
    {
        const auto part = []( const char c )
        { return ( std::isalnum( static_cast<unsigned char>( c ) ) != 0 ) || c == '_'; };
        for ( std::size_t at = haystack.find( name, from ); at != std::string::npos;
              at             = haystack.find( name, at + 1 ) )
        {
            const bool leftOk  = at == 0 || !part( haystack[at - 1] );
            const bool rightOk = at + name.size() >= haystack.size() || !part( haystack[at + name.size()] );
            if ( leftOk && rightOk )
                return at;
        }
        return std::string::npos;
    }

    constexpr const char* kSceneEnvironment =
         "Desert/Desert/Source/Engine/Graphic/Environment/SceneEnvironment.cpp";

    /// Every production source tree. Tests are deliberately NOT walked: a suite naming the member is
    /// reading it as text, which is what this file does.
    const std::vector<std::string>& ProductionRoots()
    {
        static const std::vector<std::string> roots = { "Desert/Desert/Source", "Desert/Common/Source",
                                                        "Editor/Source", "Runtime/Source", "Tools" };
        return roots;
    }

    /// A root that does not exist is skipped rather than thrown out of the test body: the error the
    /// callers are interested in is "no sources anywhere", which they assert on their own total.
    void CollectSources( const std::string& dir, std::vector<std::string>& out )
    {
        std::error_code ec;
        if ( !std::filesystem::exists( dir, ec ) )
            return;
        for ( std::filesystem::recursive_directory_iterator it( dir, ec ), end; it != end; it.increment( ec ) )
        {
            if ( ec )
                break;
            if ( !it->is_regular_file( ec ) )
                continue;
            const std::string ext = it->path().extension().string();
            if ( ext == ".cpp" || ext == ".hpp" || ext == ".h" )
                out.push_back( it->path().generic_string() );
        }
    }
} // namespace

// ------------------------------------------------------------------------------------------------
// 1. The producer: the procedural bake frees the cube AND does not hand the freed handle out.
// ------------------------------------------------------------------------------------------------
TEST( ProceduralEnvironmentCube, TheProceduralBakeFreesTheRadianceCubeAndKeepsNothingOfIt )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository root was not found from the working directory";

    const std::string source = StripComments( ReadAll( root + kSceneEnvironment ) );
    const std::string body   = BodyOf( source, "Environment EnvironmentManager::CreateProcedural" );
    ASSERT_FALSE( body.empty() ) << "EnvironmentManager::CreateProcedural is not where this suite expects it";

    constexpr const char* kFree = "Unregister( radianceHandle )";
    const std::size_t     freed = body.find( kFree );
    ASSERT_NE( freed, std::string::npos )
         << "the procedural bake is keeping its radiance cube alive. Nothing on this path samples it -- "
            "SkyboxRenderer::Render returns at the fullscreen atmosphere quad before MaterialSkybox::"
            "BindInputs is reached, and the ambient path reads IrradianceMap and PreFilteredMap by name -- "
            "so it is 96 MiB per live environment held for no reader. Free it beside the panorama.";

    // From PAST the release call, not from one character into it: the handle's own name inside
    // `Unregister( radianceHandle )` is the release, not an escape.
    EXPECT_EQ( FindIdentifier( body, "radianceHandle", freed + std::char_traits<char>::length( kFree ) ),
               std::string::npos )
         << "the procedural bake unregisters its radiance cube and then still names the handle "
            "afterwards. A freed handle must not escape this function: ImageService::Resolve answers "
            "nullptr for it (no garbage, no other image), but an Environment that CLAIMS a radiance cube "
            "it cannot produce is a claim every consumer is entitled to believe. Return "
            "Runtime::ImageHandle{} instead.";

    EXPECT_NE( body.find( "Runtime::ImageHandle{}" ), std::string::npos )
         << "the procedural Environment must state the absence of its radiance cube explicitly";
}

// ------------------------------------------------------------------------------------------------
// 2. The asymmetry: the .hdr path keeps its cube, because three live consumers read it.
// ------------------------------------------------------------------------------------------------
TEST( ProceduralEnvironmentCube, TheHdrPathKeepsItsRadianceCube )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string source = StripComments( ReadAll( root + kSceneEnvironment ) );
    const std::string body   = BodyOf( source, "Environment EnvironmentManager::Create(" );
    ASSERT_FALSE( body.empty() ) << "EnvironmentManager::Create is not where this suite expects it";

    EXPECT_EQ( body.find( "Unregister( radianceHandle )" ), std::string::npos )
         << "the .hdr path is freeing its radiance cube the way the procedural path does, and the two "
            "paths are not the same path. Here the cube has three live runtime consumers: the skybox "
            "draw (MaterialSkybox::BindInputs), the entity preview thumbnail (ScenePropertiesPanel) and "
            "the material editor's cube ball (MaterialEditorPanel). Both measured substitutes lose -- "
            "prefilter mip 0 is blockier (max delta 123/255 at the zenith) and the panorama sampled "
            "directly crawls under motion (rms 14.33 vs 10.60 under a 0.40 deg nudge).";
}

// ------------------------------------------------------------------------------------------------
// 2b. What decides that a skybox has an environment: the asset, never its file name, and never quietly.
//
// Since AF3 moved every skybox to a `.detex` container, `Create` built its environment only under
// `extension == ".hdr"` and fell to a bare `return {};` otherwise: every HDR sky rendered black with no
// IBL and the log said nothing about why. The suite has no device, so `Create` cannot be run here; what
// it can pin is the shape the defect had -- a name-based gate, and an empty return nothing announced.
// ------------------------------------------------------------------------------------------------
TEST( ProceduralEnvironmentCube, TheSkyboxAssetDecidesAndEveryEmptyEnvironmentIsAnnounced )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string source = StripComments( ReadAll( root + kSceneEnvironment ) );
    const std::string body   = BodyOf( source, "Environment EnvironmentManager::Create(" );
    ASSERT_FALSE( body.empty() ) << "EnvironmentManager::Create is not where this suite expects it";

    EXPECT_EQ( body.find( "GetFileExtension" ), std::string::npos )
         << "EnvironmentManager::Create decides by the skybox's file NAME again. Skyboxes are `.detex` "
            "containers; the container's own kind and key (FindCookedPanorama) is the one gate.";
    EXPECT_EQ( body.find( "\".hdr\"" ), std::string::npos )
         << "EnvironmentManager::Create compares against \".hdr\" -- no committed skybox has that extension.";

    const std::size_t gate = body.find( "FindCookedPanorama(" );
    ASSERT_NE( gate, std::string::npos ) << "EnvironmentManager::Create no longer asks FindCookedPanorama";
    const std::string beforeGate = body.substr( 0, gate );
    EXPECT_EQ( FindIdentifier( beforeGate, "if", 0 ), std::string::npos )
         << "something branches before FindCookedPanorama is asked -- it must be the first and only gate";
    EXPECT_EQ( FindIdentifier( beforeGate, "return", 0 ), std::string::npos )
         << "EnvironmentManager::Create returns before FindCookedPanorama is asked";

    // Every empty environment is the statement right after a LOG_ERROR: `}` or anything else before a
    // `return {};` is the silent fall-through this test exists for.
    std::size_t emptyReturns = 0;
    for ( std::size_t at = body.find( "return {};" ); at != std::string::npos;
          at             = body.find( "return {};", at + 1 ) )
    {
        ++emptyReturns;
        std::size_t end = at;
        while ( end > 0 && std::isspace( static_cast<unsigned char>( body[end - 1] ) ) != 0 )
            --end;
        ASSERT_GT( end, 0u );
        EXPECT_EQ( body[end - 1], ';' ) << "a `return {};` in EnvironmentManager::Create follows '"
                                        << body[end - 1] << "', not a logged statement:\n"
                                        << body.substr( end > 200 ? end - 200 : 0, 220 );
        if ( body[end - 1] != ';' )
            continue;
        // The statement before is a LOG_ERROR when the last LOG_ERROR before the return is followed by
        // no `;` other than its own (format strings hold `{}`, so braces cannot delimit statements).
        const std::size_t log = body.rfind( "LOG_ERROR", end );
        ASSERT_NE( log, std::string::npos ) << "an empty environment is returned and nothing is logged";
        EXPECT_EQ( body.find( ';', log ), end - 1 )
             << "an empty environment is returned without a LOG_ERROR right before it:\n"
             << body.substr( log, end - log );
    }
    EXPECT_GE( emptyReturns, 2u ) << "the two refusals (no cooked panorama, panorama did not load) are gone";
}

// ------------------------------------------------------------------------------------------------
// 3. The consumers: who is allowed to name RadianceMap at all.
// ------------------------------------------------------------------------------------------------
TEST( ProceduralEnvironmentCube, NoOneElseNamesTheRadianceCube )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // Every file here reaches the member through MaterialSkybox -- the .HDR path -- and nothing here
    // reaches it through SceneRenderer::GetEnvironment(), which answers the PROCEDURAL environment
    // whenever the sky is procedural. That is the whole relation this test states.
    const std::set<std::string> allowed = {
         "Desert/Desert/Source/Engine/Graphic/Environment/SceneEnvironment.hpp",
         "Desert/Desert/Source/Engine/Graphic/Materials/Skybox/MaterialSkybox.cpp",
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp",
         "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp",
         "Editor/Source/Editor/Panels/SceneProperties/ScenePropertiesPanel.cpp",
         // The skybox viewer shows one .hdr FILE: it reads the SkyboxService's own MaterialSkybox for that
         // asset, never a scene's composed environment, so the cube it samples is the one that is kept.
         "Editor/Source/Editor/Panels/SkyboxViewer/SkyboxViewerDocument.cpp",
    };

    std::vector<std::string> sources;
    for ( const auto& dir : ProductionRoots() )
        CollectSources( root + dir, sources );
    ASSERT_FALSE( sources.empty() ) << "no production sources were found; the roots in this file are wrong";

    std::set<std::string> found;
    for ( const auto& path : sources )
    {
        if ( StripComments( ReadAll( path ) ).find( "RadianceMap" ) == std::string::npos )
            continue;
        // Back to a repository-relative spelling, whatever "./../.." prefix RepoRoot() settled on.
        std::string relative = path;
        if ( relative.rfind( root, 0 ) == 0 )
            relative.erase( 0, root.size() );
        found.insert( relative );
    }

    std::vector<std::string> added;
    std::set_difference( found.begin(), found.end(), allowed.begin(), allowed.end(), std::back_inserter( added ) );
    std::vector<std::string> gone;
    std::set_difference( allowed.begin(), allowed.end(), found.begin(), found.end(), std::back_inserter( gone ) );

    std::string addedList;
    for ( const auto& name : added )
        addedList += "\n    " + name;
    std::string goneList;
    for ( const auto& name : gone )
        goneList += "\n    " + name;

    EXPECT_TRUE( added.empty() )
         << "a new reader of Environment::RadianceMap has appeared:" << addedList
         << "\nThe procedural sky FREES that cube at the end of its bake (it has no consumer there), so "
            "an environment obtained from SceneRenderer::GetEnvironment() or Scene::GetEnvironment() "
            "carries an EMPTY radiance handle whenever the sky is procedural, and resolving it answers "
            "nullptr. If the new site reads the .hdr path's own MaterialSkybox, add it to the list above "
            "and say so. If it reads the composed environment, it is reading a cube that is not there, "
            "and the right answer is the prefiltered cube -- not un-freeing 96 MiB per environment.";

    EXPECT_TRUE( gone.empty() ) << "these files no longer name Environment::RadianceMap:" << goneList
                                << "\nThe list above is stale -- shrink it, so it keeps meaning something.";
}

// ------------------------------------------------------------------------------------------------
// 4. The mechanism, run rather than read: a cleared handle names NOTHING.
// ------------------------------------------------------------------------------------------------
namespace
{
    /// Never constructed, never destroyed, never dereferenced -- ImageService stores the pointer and
    /// hands it back, and the identity is the whole of what it is responsible for. The same trick as
    /// Desert/Tests/Engine/EnvironmentSlotMemory, and for the same reason: Graphic::Image's constructor
    /// takes a ResourceLedger token and there is no device here to give it one.
    std::shared_ptr<Desert::Graphic::Image> Stand( std::uintptr_t address )
    {
        return { reinterpret_cast<Desert::Graphic::Image*>( address ), []( Desert::Graphic::Image* ) {} };
    }
} // namespace

TEST( ProceduralEnvironmentCube, AnEmptyHandleResolvesToNothingRatherThanToSlotZero )
{
    Desert::Runtime::ImageService service;

    // The FIRST registration takes slot 0, which is also the index a default-constructed ImageHandle
    // carries. The procedural Environment returns exactly such a handle now, so "empty" and "the first
    // image anybody registered" must not be the same answer -- generation 0 is what separates them.
    const auto first = service.Register( Stand( 0x1000 ), Desert::Runtime::ImageHandle::Type::ImageCube );
    ASSERT_TRUE( first.IsValid() );
    ASSERT_EQ( service.Resolve( first ), reinterpret_cast<Desert::Graphic::Image*>( 0x1000 ) );

    const Desert::Runtime::ImageHandle empty{};
    EXPECT_FALSE( empty.IsValid() );
    EXPECT_EQ( service.Resolve( empty ), nullptr )
         << "a default-constructed ImageHandle resolves to the image in slot 0. The procedural "
            "environment carries exactly such a handle for its freed radiance cube, so every consumer "
            "that resolves it would be handed an unrelated cube -- the 'plausible pointer instead of an "
            "error' shape. Handle::IsValid() is generation != 0 and HandlePool::Allocate hands out "
            "generation 1 first; something has changed one of those two.";
}

TEST( ProceduralEnvironmentCube, ReleasingNothingReleasesNothing )
{
    Desert::Runtime::ImageService service;

    const auto first = service.Register( Stand( 0x1000 ), Desert::Runtime::ImageHandle::Type::ImageCube );
    ASSERT_TRUE( first.IsValid() );

    // The three cubes of a previous environment are released together, unconditionally, by
    // SkyboxRenderer -- and one of the three is now routinely
    // empty. Handing an empty handle to Unregister is therefore a NORMAL event, not an error, and must
    // disturb nothing.
    service.Unregister( Desert::Runtime::ImageHandle{} );

    EXPECT_EQ( service.Resolve( first ), reinterpret_cast<Desert::Graphic::Image*>( 0x1000 ) )
         << "unregistering an empty handle released the image in slot 0";
}

TEST( ProceduralEnvironmentCube, AFreedCubeIsNotResolvableAndItsNeighbourIsUntouched )
{
    Desert::Runtime::ImageService service;

    const auto radiance    = service.Register( Stand( 0x1000 ), Desert::Runtime::ImageHandle::Type::ImageCube );
    const auto prefiltered = service.Register( Stand( 0x2000 ), Desert::Runtime::ImageHandle::Type::ImageCube );

    service.Unregister( radiance );

    EXPECT_EQ( service.Resolve( radiance ), nullptr );
    EXPECT_EQ( service.Resolve( prefiltered ), reinterpret_cast<Desert::Graphic::Image*>( 0x2000 ) )
         << "freeing the bake's radiance cube took the prefiltered cube with it";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
