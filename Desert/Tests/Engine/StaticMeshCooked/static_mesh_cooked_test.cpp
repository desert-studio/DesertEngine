// A COOKED MESH THAT LOADS CARRIES WHAT ITS FILE CARRIES — a suite about a RELATION between a file and
// the asset built from it, not about a call.
//
// The relation: A MESH WHOSE FILE HOLDS K SUBMESHES HAS K SUBMESHES AFTER A SUCCESSFUL LOAD, AND A LOAD
// THAT WOULD PRODUCE NONE IS NOT A SUCCESS. Both halves matter and only the second is unusual: an asset
// with zero submeshes is a perfectly constructible asset, so every check written about the asset ALONE
// passes on it. What it is not is drawable — MeshECSSystem iterates GetSubmeshes() and nothing else — so a
// successful empty load is a silent wrong answer in the sense DC 1.4 names.
//
// WHAT WAS BROKEN, WITH THE NUMBERS. `Cooked/Meshes/base.stmesh` carries 1 submesh, 105 317 vertices and
// 120 000 triangles. `MeshService::Get` answered with a Mesh holding ZERO submeshes while `EnsureLoaded`
// reported success, and `TryFrameMesh` — which reads the ASSET — measured the model correctly at the same
// moment. Two different objects behind one handle: `ComponentRegistry` called `MeshService::Register` and
// only THEN `Load()`, and `AssetManager::CreateAsset` deduplicates on a spelling-independent key, so the
// scene's relative path handed back AssetPreloader's UNPARSED shell for the absolute spelling of the same
// file. The GPU mesh was therefore built from 0 vertices and cached under the handle; the `Load()` on the
// next line filled the asset, which nothing rebuilt the mesh from. Measured on the reproducer scene: one
// `Register` with 0 submeshes on the asset, then 91 cache hits answering 0 in a single 90-frame run.
//
// WHY THIS SUITE EXISTS AT ALL, WHICH IS THE OTHER HALF OF THE DEFECT. Until 2026-09-08 not one scene in
// this repository named a `.stmesh`, and no suite parsed one. The cooked STATIC mesh path was reachable
// only by importing a file by hand — so a break in it was invisible for as long as nobody imported
// anything. `Editor/Cooked/Meshes/StaticProbe.stmesh` is the answer: a real cooked static mesh, 7 KB,
// committed, placed by `Editor/Resources/Assets/Scenes/M10_MeshSlot.desce`, and parsed here.
//
// WHY THE PROBE HAS TWO SUBMESHES. One submesh cannot distinguish "the count survived" from "the count is
// always one", and a submesh at a NON-ZERO VertexOffset/IndexOffset is the only kind whose ranges can be
// wrong — the first submesh of any mesh starts at 0 and looks correct however the offsets are computed.
//
// THE SERVICE-LEVEL HALF, AND WHY IT IS A CENSUS OVER SOURCE TEXT. `MeshService::BuildAndCache` is the one
// place a runtime mesh is built from an asset, and it is where both guards against the defect above live:
// parse before build, then compare what was built against what the asset holds. It is also structurally
// UNTESTABLE by this project's sweep — `MeshService.cpp` is compiled by exactly ONE makefile, `Desert.make`,
// and no test suite links `libDesert.a`, because that pulls in Vulkan and the whole renderer. Measured on
// 2026-09-15 by restoring the pre-fix body of that function in the live tree: `M10_MeshSlot.desce` went
// from the two probe boxes to a FLAT GREY frame (75 807 of 560 560 pixels, contrast 0.675 -> 0.004), not one
// line appeared in the log, and all THIRTEEN suites this task was told to run stayed green — including this
// one. A property no runtime test on this machine can observe is gated over the source text instead; that is
// the same instrument the shader-graph argument-order defect needed, and for the same reason.
//
// So the two tests at the bottom of this file pin the ORDER of that function's steps and the fact that there
// is only one of it. Everything above them covers the asset-level half, which is where the numbers K and N
// live and which the suite can execute directly.

#include <gtest/gtest.h>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using Desert::Assets::AssetManager;
using Desert::Assets::AssetPriority;
using Desert::Assets::StaticMeshAsset;
using Desert::Assets::Serialization::MeshAssetData;

namespace
{
    // The shipped probe, by the path the scene stores it under. Its handle is derived from that
    // project-relative spelling, so the constant below is the same number on every machine.
    constexpr const char*   kProbeCookedPath    = "Cooked/Meshes/StaticProbe.stmesh";
    constexpr std::uint64_t kProbeMeshHandle    = 10064960323608083546ull;
    constexpr std::size_t   kProbeSubmeshes     = 2;
    constexpr std::size_t   kProbeVertices      = 48; // two boxes, 24 per-face vertices each
    constexpr std::size_t   kProbeTriangles     = 24; // 12 per box
    constexpr const char*   kProbeFirstSubmesh  = "ProbeCube";
    constexpr const char*   kProbeSecondSubmesh = "ProbePillar";

    // The repository root, found by walking up from the test binary's working directory — the same way
    // Tests/Engine/ShippedShaderPasses and Tests/Engine/ShaderCacheKey locate shipped content.
    std::filesystem::path RepositoryRoot()
    {
        std::filesystem::path here = std::filesystem::current_path();
        for ( int up = 0; up < 8 && !std::filesystem::exists( here / "Editor" / "Cooked" / "Meshes" ); ++up )
            here = here.parent_path();
        return here;
    }

    std::filesystem::path ProbeFile()
    {
        return RepositoryRoot() / "Editor" / kProbeCookedPath;
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    // What the FILE says, read straight through reflect-cpp — the K side of the relation, obtained without
    // going through the asset class whose behaviour is under test.
    const MeshAssetData& ProbeFileContents()
    {
        static const MeshAssetData data = []
        {
            // THROUGH THE SAME READER THE ENGINE USES (B11). The probe is a binary container now, so a
            // bare `rfl::json::read` here would fail on the shipped file while the engine loaded it
            // perfectly — a suite reading the fixture by a route the engine does not take.
            const auto parsed =
                 Desert::Assets::Serialization::ReadMeshAssetData( ReadFile( ProbeFile() ), ProbeFile().string() );
            EXPECT_TRUE( parsed.IsSuccess() ) << "the shipped probe does not parse as a cooked mesh: "
                                              << ( parsed.IsSuccess() ? std::string{} : parsed.GetError() );
            return parsed.IsSuccess() ? parsed.GetValue() : MeshAssetData{};
        }();
        return data;
    }

    void WriteText( const std::filesystem::path& path, const std::string& text )
    {
        std::filesystem::create_directories( path.parent_path() );
        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        out << text;
    }

    // One scratch directory per test, removed with it, so a file left behind cannot make the next one pass.
    class ScratchDir
    {
    public:
        explicit ScratchDir( const std::string& name )
             : m_Dir( std::filesystem::temp_directory_path() / ( "DesertStaticProbe_" + name ) )
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Dir, ec );
            std::filesystem::create_directories( m_Dir, ec );
        }

        ~ScratchDir()
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Dir, ec );
        }

        ScratchDir( const ScratchDir& )            = delete;
        ScratchDir& operator=( const ScratchDir& ) = delete;

        std::string Write( const char* stem, const MeshAssetData& data ) const
        {
            const auto path = m_Dir / ( std::string( stem ) + ".stmesh" );
            WriteText( path, rfl::json::write( data ) );
            return path.generic_string();
        }

    private:
        std::filesystem::path m_Dir;
    };
} // namespace

// THE SHIPPED PROBE IS REACHABLE AND IS WHAT THIS SUITE THINKS IT IS. Every other test here reads it, so a
// probe that moved would otherwise make them all pass vacuously on an empty MeshAssetData.
TEST( StaticMeshCooked, TheShippedProbeIsOnDiskAndCarriesTheGeometryItClaims )
{
    ASSERT_TRUE( std::filesystem::exists( ProbeFile() ) )
         << "no cooked probe at " << ProbeFile().string()
         << " — M10_MeshSlot.desce names it and would render nothing.";

    const auto& file = ProbeFileContents();
    EXPECT_FALSE( file.IsSkinned );
    EXPECT_EQ( file.Submeshes.size(), kProbeSubmeshes );
    EXPECT_EQ( file.StaticVertices.size(), kProbeVertices );
    EXPECT_EQ( file.Indices.size(), kProbeTriangles );
    ASSERT_EQ( file.Submeshes.size(), kProbeSubmeshes );
    EXPECT_EQ( file.Submeshes[0].Name, kProbeFirstSubmesh );
    EXPECT_EQ( file.Submeshes[1].Name, kProbeSecondSubmesh );
    EXPECT_GT( file.Submeshes[1].VertexOffset, 0u )
         << "the second submesh must start past the first, or the probe cannot tell a mesh whose offsets "
            "are computed wrongly from one whose offsets are right.";
}

// EVERY SUBMESH INDEXES GEOMETRY THAT EXISTS. A range that runs off the end of the arrays is the defect
// shape the LOD builder walks straight into (it reads baseIndices[IndexOffset/3 + t] and
// vertices[VertexOffset + v]) — and both sides look individually well-formed, which is why the RELATION is
// what gets asserted. `IndexOffset`/`IndexCount` are in uint32 index units and the triangle array is in
// triplets; that factor of three is exactly the kind of disagreement this catches.
TEST( StaticMeshCooked, EverySubmeshRangeLiesInsideTheMeshItIndexes )
{
    const auto& file = ProbeFileContents();
    ASSERT_FALSE( file.Submeshes.empty() );

    for ( const auto& submesh : file.Submeshes )
    {
        EXPECT_GT( submesh.VertexCount, 0u ) << submesh.Name;
        EXPECT_GT( submesh.IndexCount, 0u ) << submesh.Name;
        EXPECT_EQ( submesh.IndexCount % 3u, 0u ) << submesh.Name << ": index ranges are whole triangles";
        EXPECT_EQ( submesh.IndexOffset % 3u, 0u ) << submesh.Name << ": index ranges start on a triangle";
        EXPECT_LE( static_cast<std::size_t>( submesh.VertexOffset ) + submesh.VertexCount,
                   file.StaticVertices.size() )
             << submesh.Name;
        EXPECT_LE( ( static_cast<std::size_t>( submesh.IndexOffset ) + submesh.IndexCount ) / 3u,
                   file.Indices.size() )
             << submesh.Name;
    }
}

// THE RELATION ITSELF: FILE CARRIES K, THE LOADED ASSET HAS K. Asserted against the file's own count rather
// than against a number typed here, so re-cooking the probe with more parts extends the test instead of
// breaking it.
TEST( StaticMeshCooked, LoadPutsExactlyWhatTheFileCarries )
{
    const auto& file = ProbeFileContents();

    StaticMeshAsset asset( AssetPriority::Low, Common::Filepath( ProbeFile() ) );
    ASSERT_FALSE( asset.IsReadyForUse() );

    const auto loaded = asset.Load();
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();

    EXPECT_TRUE( asset.IsReadyForUse() );
    EXPECT_EQ( asset.GetSubmeshes().size(), file.Submeshes.size() );
    EXPECT_EQ( asset.GetVertices().size(), file.StaticVertices.size() );
    EXPECT_EQ( asset.GetIndices().size(), file.Indices.size() );

    // One material handle per submesh. The two vectors are written in the same loop and read at the same
    // index by GetMaterialHandle, so a mesh whose counts drift indexes the wrong material or out of range.
    EXPECT_EQ( asset.GetMaterialHandles().size(), asset.GetSubmeshes().size() );
}

// A SHELL ANSWERS ZERO, AND THAT IS THE STATE THE DEFECT BUILT FROM. Pinned deliberately: an implementation
// that made an unparsed shell report the file's submesh count would satisfy the test above while leaving
// `MeshService::Register` building a mesh out of nothing.
TEST( StaticMeshCooked, AnUnparsedShellReportsNoSubmeshesAndSaysItIsNotReady )
{
    AssetManager manager;

    // Exactly what AssetPreloader does for every cooked mesh.
    auto shell = manager.CreateAsset<StaticMeshAsset>( AssetPriority::Low, ProbeFile().generic_string(),
                                                       /*loadAfterCreate=*/false );
    ASSERT_TRUE( shell );
    EXPECT_FALSE( shell->IsReadyForUse() );
    EXPECT_TRUE( shell->GetSubmeshes().empty() );
    EXPECT_TRUE( shell->GetVertices().empty() );

    ASSERT_TRUE( shell->EnsureLoaded( manager ).IsSuccess() );
    EXPECT_TRUE( shell->IsReadyForUse() );
    EXPECT_EQ( shell->GetSubmeshes().size(), ProbeFileContents().Submeshes.size() );
}

// SUCCESS AND EMPTINESS ARE DIFFERENT ANSWERS (DC 1.4). A cooked file with vertices but no submeshes draws
// nothing; if Load reported success on it, "loaded" and "loaded, and useless" would be indistinguishable to
// every caller — which is the state `MeshService::Get` was in for the whole of this defect's life.
TEST( StaticMeshCooked, AFileWithNoSubmeshesIsRefusedRatherThanLoadedEmpty )
{
    const ScratchDir scratch( "empty" );

    MeshAssetData data     = ProbeFileContents();
    const auto    vertices = data.StaticVertices.size();
    data.Submeshes.clear(); // vertices and indices intact — only the drawable parts are gone
    ASSERT_GT( vertices, 0u );

    StaticMeshAsset asset( AssetPriority::Low, Common::Filepath( scratch.Write( "NoSubmeshes", data ) ) );

    const auto loaded = asset.Load();
    EXPECT_FALSE( loaded.IsSuccess() )
         << "a cooked mesh with zero submeshes loaded 'successfully'; nothing downstream can tell that "
            "apart from a mesh that works.";
    EXPECT_FALSE( asset.IsReadyForUse() )
         << "a refused load must leave the asset unready, or EnsureLoaded will never try again.";

    // The refusal has to be readable by a human holding a broken cook: the file and both counts.
    EXPECT_NE( loaded.GetError().find( "ZERO submeshes" ), std::string::npos ) << loaded.GetError();
    EXPECT_NE( loaded.GetError().find( "NoSubmeshes" ), std::string::npos ) << loaded.GetError();
}

// A REFUSED LOAD LEAVES NOTHING BEHIND. Load clears its five vectors before it fills them, so a refusal
// placed after the clears would empty an asset that was previously good — an eviction nobody asked for.
TEST( StaticMeshCooked, ARefusedLoadDoesNotDestroyWhatTheAssetAlreadyHeld )
{
    const ScratchDir scratch( "keep" );

    const auto      path = scratch.Write( "Probe", ProbeFileContents() );
    StaticMeshAsset asset( AssetPriority::Low, Common::Filepath( path ) );
    ASSERT_TRUE( asset.Load().IsSuccess() );
    const auto submeshes = asset.GetSubmeshes().size();
    const auto vertices  = asset.GetVertices().size();
    ASSERT_GT( submeshes, 0u );

    // The file goes bad underneath it — a truncated cook, a half-written re-import.
    MeshAssetData broken = ProbeFileContents();
    broken.Submeshes.clear();
    WriteText( path, rfl::json::write( broken ) );

    EXPECT_FALSE( asset.Load().IsSuccess() );
    EXPECT_EQ( asset.GetSubmeshes().size(), submeshes );
    EXPECT_EQ( asset.GetVertices().size(), vertices );
}

// THE TWO ROUTES REACH THE SAME ASSET. Eager (CreateAsset loads) and deferred (a shell plus EnsureLoaded)
// are two spellings of "this mesh is loaded", and this defect was one of them producing something usable
// and the other not. Comparing them to each other is what catches the next divergence.
TEST( StaticMeshCooked, TheEagerAndDeferredRoutesReachTheSameGeometry )
{
    const ScratchDir scratch( "routes" );

    // Two copies at two paths so the manager keeps them as two records.
    const auto eagerPath    = scratch.Write( "Eager", ProbeFileContents() );
    const auto deferredPath = scratch.Write( "Deferred", ProbeFileContents() );

    AssetManager manager;
    auto eager = manager.CreateAsset<StaticMeshAsset>( AssetPriority::Low, eagerPath, /*loadAfterCreate=*/true );
    ASSERT_TRUE( eager );

    auto deferred =
         manager.CreateAsset<StaticMeshAsset>( AssetPriority::Low, deferredPath, /*loadAfterCreate=*/false );
    ASSERT_TRUE( deferred );
    ASSERT_TRUE( deferred->EnsureLoaded( manager ).IsSuccess() );

    EXPECT_EQ( eager->IsReadyForUse(), deferred->IsReadyForUse() );
    EXPECT_EQ( eager->GetSubmeshes().size(), deferred->GetSubmeshes().size() );
    EXPECT_EQ( eager->GetVertices().size(), deferred->GetVertices().size() );
    EXPECT_EQ( eager->GetMaterialHandles().size(), deferred->GetMaterialHandles().size() );
}

// A SECOND LOAD IS A RELOAD, NOT AN APPEND. Every vector Load fills is cleared first; A7 found the material
// handles were not, which doubled them and left STALE handles at the indices every draw reads. Asserted as
// a relation against the file so it cannot be satisfied by clearing one vector and forgetting another.
TEST( StaticMeshCooked, ReloadingLeavesTheSameCountsRatherThanDoubledOnes )
{
    const auto& file = ProbeFileContents();

    StaticMeshAsset asset( AssetPriority::Low, Common::Filepath( ProbeFile() ) );
    ASSERT_TRUE( asset.Load().IsSuccess() );
    ASSERT_TRUE( asset.Load().IsSuccess() );

    EXPECT_EQ( asset.GetSubmeshes().size(), file.Submeshes.size() );
    EXPECT_EQ( asset.GetVertices().size(), file.StaticVertices.size() );
    EXPECT_EQ( asset.GetIndices().size(), file.Indices.size() );
    EXPECT_EQ( asset.GetMaterialHandles().size(), file.Submeshes.size() );
}

// AN UNLOADED ASSET IS EMPTY AND SAYS SO. Unload's whole contract is that IsReadyForUse goes false, because
// that flag is what EnsureLoaded asks before deciding to parse; an emptied asset still reporting "ready" is
// one nobody will ever reload — and a mesh with no submeshes reporting "ready" is this task's defect again.
TEST( StaticMeshCooked, UnloadEmptiesTheAssetAndStopsItClaimingToBeReady )
{
    StaticMeshAsset asset( AssetPriority::Low, Common::Filepath( ProbeFile() ) );
    ASSERT_TRUE( asset.Load().IsSuccess() );
    ASSERT_FALSE( asset.GetSubmeshes().empty() );

    ASSERT_TRUE( asset.Unload().IsSuccess() );
    EXPECT_FALSE( asset.IsReadyForUse() );
    EXPECT_TRUE( asset.GetSubmeshes().empty() );
    EXPECT_TRUE( asset.GetVertices().empty() );
    EXPECT_TRUE( asset.GetMaterialHandles().empty() );

    // And it comes back, which is what makes eviction eviction rather than data loss.
    ASSERT_TRUE( asset.Load().IsSuccess() );
    EXPECT_EQ( asset.GetSubmeshes().size(), ProbeFileContents().Submeshes.size() );
}

// THE SHIPPED PROBE'S IDENTITY. The scene names the mesh by path, and every service map keys on the handle
// derived from that path. The number is written down here so a rename or a move fails loudly instead of
// silently emptying the one scene that covers this path.
TEST( StaticMeshCooked, TheShippedProbeKeepsTheIdentityTheSceneNamesItBy )
{
    EXPECT_EQ( static_cast<std::uint64_t>( Common::AssetHandle::FromCookedPath( kProbeCookedPath ) ),
               kProbeMeshHandle )
         << "the probe mesh's path-derived handle changed; M10_MeshSlot.desce would resolve to no mesh.";
}

// ---------------------------------------------------------------------------------------------------------
// THE SERVICE-LEVEL GUARDS, READ OUT OF THE SOURCE. See the note at the top of this file for why these two
// are censuses and not executions, and for the frame that measured what their removal costs.

namespace
{
    // The body of one function definition in `MeshService.cpp`. Every function in that file is spelled
    // `<return type> MeshService::<name>(`, so the next occurrence of `MeshService::` after the opening one
    // is the start of the next definition — which makes the slice between them this function and nothing
    // else. Returned empty if the function is gone, which the caller reports as its own failure rather than
    // passing vacuously on an empty string.
    std::string MeshServiceFunctionBody( const std::string& name )
    {
        const std::string source = ReadFile( RepositoryRoot() / "Desert" / "Desert" / "Source" / "Engine" /
                                             "Runtime" / "Services" / "Mesh" / "MeshService.cpp" );
        const auto        start  = source.find( "MeshService::" + name + "(" );
        if ( start == std::string::npos )
        {
            return {};
        }
        const auto next = source.find( "MeshService::", start + 1 );
        return source.substr( start, next == std::string::npos ? std::string::npos : next - start );
    }
} // namespace

// PARSE BEFORE BUILD, THEN COMPARE — as an ORDERING, because each step on its own is present in the broken
// version too. The defect was not a missing call; it was `MeshFactory::Create` running against an asset
// nobody had parsed yet, and the empty mesh that produced being cached under a live handle for the rest of
// the process. Order is the property, so order is what is asserted.
TEST( StaticMeshCooked, TheServiceParsesBeforeItBuildsAndComparesBeforeItCaches )
{
    const std::string body = MeshServiceFunctionBody( "BuildAndCache" );
    ASSERT_FALSE( body.empty() ) << "MeshService::BuildAndCache is gone from MeshService.cpp. It is the one "
                                    "place a runtime mesh is built from an asset; if the build moved, move "
                                    "this census with it rather than deleting it.";

    const auto ensureLoaded = body.find( "EnsureLoaded(" );
    const auto create       = body.find( "MeshFactory::Create(" );
    const auto compare      = body.find( "GetSubmeshes().size() != " );
    const auto cache        = body.find( "m_Meshes[handle] = " );

    ASSERT_NE( create, std::string::npos ) << "BuildAndCache no longer builds anything.";
    ASSERT_NE( cache, std::string::npos ) << "BuildAndCache no longer caches anything.";

    EXPECT_NE( ensureLoaded, std::string::npos )
         << "BuildAndCache does not parse the asset at all. Building from an UNPARSED shell is the defect "
            "itself: MeshFactory copies whatever the asset holds at that instant, so the runtime mesh gets "
            "zero vertices and zero submeshes, and gets cached under the handle for the life of the "
            "process. `Cooked/Meshes/base.stmesh` carried 1 submesh, 105 317 vertices and 120 000 "
            "triangles; `Get` answered 0 submeshes ninety-one times in one 90-frame run.";
    EXPECT_LT( ensureLoaded, create )
         << "BuildAndCache builds the runtime mesh BEFORE it parses the asset. That is the original defect "
            "verbatim, and nothing else in this repository can see it: no test suite compiles "
            "MeshService.cpp.";

    EXPECT_NE( compare, std::string::npos )
         << "BuildAndCache no longer compares the submesh count of the mesh it built against the asset it "
            "built it from. Both sides are individually well-formed — that is exactly why the comparison "
            "has to exist: an empty mesh is a perfectly constructible mesh, and caching one is "
            "irreversible because `Get` never asks the asset again.";
    EXPECT_LT( create, compare ) << "the comparison happens before the build, so it compares nothing.";
    EXPECT_LT( compare, cache )
         << "the mesh is cached BEFORE the comparison, so a mismatch is detected after the damage is "
            "permanent. The whole value of the check is that a failed build caches NOTHING.";
}

// ONE BUILD SITE, WHICH IS WHAT MAKES THE ORDERING ABOVE SUFFICIENT. The eager route (`Register`) and the
// lazy route (`Get`) disagreed once already; they are one function now, and a second `MeshFactory::Create`
// anywhere in this file would be a route past both guards that still looks like a built mesh.
TEST( StaticMeshCooked, TheServiceHasExactlyOnePlaceThatBuildsAMeshFromAnAsset )
{
    const std::string source = ReadFile( RepositoryRoot() / "Desert" / "Desert" / "Source" / "Engine" / "Runtime" /
                                         "Services" / "Mesh" / "MeshService.cpp" );
    ASSERT_FALSE( source.empty() ) << "could not read MeshService.cpp";

    std::size_t builds = 0;
    for ( std::size_t at = source.find( "MeshFactory::Create(" ); at != std::string::npos;
          at             = source.find( "MeshFactory::Create(", at + 1 ) )
    {
        ++builds;
    }
    EXPECT_EQ( builds, 1u ) << "MeshService.cpp builds a mesh from an asset in " << builds
                            << " places. Every one of them owes the asset a parse first and the result a "
                               "comparison afterwards; the reason there is one is so that neither can be "
                               "forgotten in the other.";

    // And both routes must reach it through the same door, or the eager and lazy paths can drift apart
    // again — which is how the defect reached a shipped scene in the first place.
    std::size_t routes = 0;
    for ( std::size_t at = source.find( "BuildAndCache(" ); at != std::string::npos;
          at             = source.find( "BuildAndCache(", at + 1 ) )
    {
        ++routes;
    }
    EXPECT_GE( routes, 3u ) << "BuildAndCache is named " << routes
                            << " times (its definition plus one call from Register and one from Get). Fewer "
                               "means a route builds its mesh some other way.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
