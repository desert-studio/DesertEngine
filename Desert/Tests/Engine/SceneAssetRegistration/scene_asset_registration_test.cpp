// Ф6 — ONE RELATION, ASSERTED TWICE, BECAUSE IT CAN FAIL IN TWO PLACES.
//
//     EVERY ASSET A LOADED SCENE REFERENCES IS REGISTERED IN ITS SERVICE, ON EVERY ROUTE THAT
//     RESOLVES THE REFERENCE.
//
// The defect this is about is the tree's most-repeated shape: both ends of a chain look right and the
// middle link silently drops half the work. `ComponentRegistry`'s material and mesh branches resolved a
// scene's reference as FIND-ELSE-CREATE and registered with the service only on the CREATE route. A
// lookup that returns an existing record is a correct lookup; registering a record you just made is a
// correct registration; the disagreement is that a reference to an asset SOMEBODY ELSE had already
// created resolved to a live handle no service could answer for.
//
// It never showed, because `AssetPreloader::PreloadCookedAssetsAndMaterials` registers every mesh and
// material under the two content roots it walks, and `EditorLayer::OnUpdate` refuses to load a scene
// until the startup stages have finished. THAT IS A SAFETY NET, NOT A GUARANTEE — it is stated nowhere
// the parse can read, it covers only what those two roots contain, and it leaves with the first refactor
// by somebody who does not know it is load-bearing.
//
// WHY TWO ASSERTIONS AND NOT ONE. The relation can be broken two ways and they need different tests:
//
//   1. THE RULE ITSELF could be written to treat the routes differently. `ResolveSceneReference` is the
//      one place the rule lives, it is a pure template, and fakes make both routes reachable without a
//      device. Asserted as an AGREEMENT between the routes (§4 of `desert-engine-verify`) and not as two
//      separate facts, because two separate facts is exactly how this project found one defect four
//      times.
//
//   2. THE CALL SITES could stop using it — most cheaply by doing what the task forbade in as many
//      words: adding a registration to the found branch and leaving the created branch's one in place.
//      Two registration sites is the same disagreement waiting to happen, so the census below asserts
//      that each of the three services the resolver touches is reached from EXACTLY ONE place, and that
//      BOTH spellings of a reference (a path and a stable handle) go through it. On the tree before this
//      task the counts were 2 for meshes, 3 for materials and 1 for textures — and the texture case is
//      why the census needs both halves, because its single site served only one of the two spellings.
//
// NO GPU, NO WINDOW, NO ASSET MANAGER. The rule is a template over three callables and the census is a
// text scan, so this suite runs anywhere the sweep does.

#include <gtest/gtest.h>

#include <Engine/Core/Serialize/AssetReferenceResolve.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using Desert::Core::Serialize::ReferenceOrigin;
using Desert::Core::Serialize::ResolveSceneReference;

namespace
{
    // A record stands in for `Assets::Asset<MeshAsset>` / `Asset<MaterialAsset>`: a shared_ptr-like thing
    // whose falsiness means "the registry does not hold this file". Nothing about the relation depends on
    // what an asset IS, so nothing about an asset is compiled in here.
    struct Record
    {
        int Id = 0;

        explicit operator bool() const
        {
            return Id != 0;
        }
    };

    // What a service would have been told.
    struct Ledger
    {
        std::vector<std::pair<int, ReferenceOrigin>> Registered;

        size_t CountFor( ReferenceOrigin origin ) const
        {
            size_t n = 0;
            for ( const auto& entry : Registered )
                if ( entry.second == origin )
                    ++n;
            return n;
        }
    };

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // THE CENSUS COUNTS CODE, SO IT HAS TO STOP COUNTING PROSE. Every one of these files now carries a
    // comment QUOTING the call it used to make ("was `if ( !GetMaterialService()->Get( h ) ) ...`"), which
    // is exactly the sentence the next reader needs and exactly the text a naive scan reports as a
    // violation. It caught this suite the first time it ran. String literals are stepped over as well, so
    // a URL's `//` cannot swallow the rest of a line of real code.
    std::string WithoutCommentsAndStrings( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        for ( size_t i = 0; i < source.size(); )
        {
            if ( source.compare( i, 2, "//" ) == 0 )
            {
                while ( i < source.size() && source[i] != '\n' )
                    ++i;
            }
            else if ( source.compare( i, 2, "/*" ) == 0 )
            {
                i = source.find( "*/", i );
                i = ( i == std::string::npos ) ? source.size() : i + 2;
            }
            else if ( source[i] == '"' )
            {
                out += ' ';
                for ( ++i; i < source.size() && source[i] != '"'; ++i )
                {
                    if ( source[i] == '\\' )
                        ++i;
                }
                if ( i < source.size() )
                    ++i;
            }
            else
            {
                out += source[i++];
            }
        }
        return out;
    }

    // `MakeAssetResolver` bridges an AssetHandle field BOTH ways, and only one of them resolves a
    // reference. `ToPath` turns a handle a component already holds into a string for the file; it looks a
    // record up and registers nothing, correctly, because nothing is being resolved. Slicing it out is
    // what makes "every lookup is followed by a registration" a true statement rather than a rule with an
    // exception nobody wrote down. Files without a `ToPath` are returned unchanged.
    std::string WithoutTheSerializationDirection( const std::string& source )
    {
        const size_t begin = source.find( "r.ToPath = " );
        if ( begin == std::string::npos )
            return source;
        const size_t end = source.find( "r.FromPath = ", begin );
        if ( end == std::string::npos )
            return source;
        return source.substr( 0, begin ) + source.substr( end );
    }

    size_t CountOccurrences( const std::string& haystack, const std::string& needle )
    {
        size_t n = 0;
        for ( size_t at = haystack.find( needle ); at != std::string::npos;
              at        = haystack.find( needle, at + needle.size() ) )
            ++n;
        return n;
    }

    // HOW MANY TIMES A FILE REACHES ONE OF THE THREE SERVICES AT ALL — not how many times it calls a
    // particular method on one. Counting `->Register(` would pass a file that registered eagerly in one
    // branch and lazily in another, and it would also pass a `->Get()` used as a guard, which is the shape
    // this whole line of work removed (the guard BUILT the thing it was asking about).
    size_t ServiceTouches( const std::string& source, const std::string& accessor )
    {
        return CountOccurrences( source, accessor );
    }

    // Every way a file can ask the registry for a mesh record, and every way it can hand one to the
    // registrar. The relation asserted below is between these two numbers, in the same file.
    size_t MeshLookups( const std::string& source )
    {
        return CountOccurrences( source, "FindByPath<Assets::MeshAsset>" ) +
               CountOccurrences( source, "FindByHandle<Assets::MeshAsset>" );
    }

    size_t MeshRegistrations( const std::string& source )
    {
        return CountOccurrences( source, "EnsureMeshRegistered(" ) +
               CountOccurrences( source, "EnsureMeshDrawable(" );
    }

    size_t MaterialLookups( const std::string& source )
    {
        return CountOccurrences( source, "FindByPath<Assets::MaterialAsset>" ) +
               CountOccurrences( source, "FindByHandle<Assets::MaterialAsset>" ) +
               CountOccurrences( source, "FindByPath<Assets::SurfaceMaterialAsset>" );
    }

    size_t MaterialRegistrations( const std::string& source )
    {
        return CountOccurrences( source, "EnsureMaterialRegistered(" );
    }

    // The three files that resolve an asset reference as find-else-create, and therefore the three that
    // had independently written the same rule.
    const std::vector<std::string>& ResolverFiles()
    {
        static const std::vector<std::string> files = {
             "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp",
             "Editor/Source/Editor/Import/MeshDnD.cpp",
             "Editor/Source/Editor/Widgets/ThumbnailSubject.cpp",
        };
        return files;
    }

    constexpr const char* kRegistrar = "Desert/Desert/Source/Engine/Runtime/Services/AssetServiceRegistration.cpp";

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// 1. The rule: the two routes are not told apart.
// ─────────────────────────────────────────────────────────────────────────────────────────────────

TEST( SceneAssetRegistration, AFoundRecordIsRegistered )
{
    Ledger ledger;

    const Record resolved = ResolveSceneReference(
         [] { return Record{ 7 }; }, [] { return Record{ 0 }; }, // must not be reached
         [&ledger]( const Record& r, ReferenceOrigin o ) { ledger.Registered.push_back( { r.Id, o } ); } );

    EXPECT_EQ( resolved.Id, 7 );
    ASSERT_EQ( ledger.Registered.size(), 1u );
    EXPECT_EQ( ledger.Registered[0].first, 7 );
    EXPECT_EQ( ledger.Registered[0].second, ReferenceOrigin::Found );
}

TEST( SceneAssetRegistration, ACreatedRecordIsRegistered )
{
    Ledger ledger;

    const Record resolved = ResolveSceneReference( [] { return Record{ 0 }; }, [] { return Record{ 9 }; },
                                                   [&ledger]( const Record& r, ReferenceOrigin o )
                                                   { ledger.Registered.push_back( { r.Id, o } ); } );

    EXPECT_EQ( resolved.Id, 9 );
    ASSERT_EQ( ledger.Registered.size(), 1u );
    EXPECT_EQ( ledger.Registered[0].first, 9 );
    EXPECT_EQ( ledger.Registered[0].second, ReferenceOrigin::Created );
}

// THE RELATION, stated as the agreement between the two routes rather than as two facts about them. A
// change that registers on one route and not the other passes both tests above one at a time and fails
// here — which is the whole reason this project stopped asserting sides.
TEST( SceneAssetRegistration, BothRoutesRegisterTheSameNumberOfTimes )
{
    Ledger ledger;

    ResolveSceneReference( [] { return Record{ 1 }; }, [] { return Record{ 0 }; },
                           [&ledger]( const Record& r, ReferenceOrigin o )
                           { ledger.Registered.push_back( { r.Id, o } ); } );
    ResolveSceneReference( [] { return Record{ 0 }; }, [] { return Record{ 2 }; },
                           [&ledger]( const Record& r, ReferenceOrigin o )
                           { ledger.Registered.push_back( { r.Id, o } ); } );

    EXPECT_EQ( ledger.CountFor( ReferenceOrigin::Found ), ledger.CountFor( ReferenceOrigin::Created ) );
    EXPECT_EQ( ledger.Registered.size(), 2u );
}

// A reference that names nothing registers nothing — and, just as importantly, does not report anything
// from inside the rule. The caller owns that message because only the caller knows the asset type and
// the spelling that failed (DC 1.4).
TEST( SceneAssetRegistration, AReferenceThatResolvesToNothingRegistersNothing )
{
    Ledger ledger;

    const Record resolved = ResolveSceneReference( [] { return Record{ 0 }; }, [] { return Record{ 0 }; },
                                                   [&ledger]( const Record& r, ReferenceOrigin o )
                                                   { ledger.Registered.push_back( { r.Id, o } ); } );

    EXPECT_FALSE( static_cast<bool>( resolved ) );
    EXPECT_TRUE( ledger.Registered.empty() );
}

// The Found route must not run `create`, and the Created route must not run `find` twice. Both are what
// makes the rule affordable to apply to EVERY reference in a scene: the common case is one map lookup.
TEST( SceneAssetRegistration, TheRouteNotTakenIsNotRun )
{
    int finds = 0, creates = 0;

    ResolveSceneReference(
         [&finds]
         {
             ++finds;
             return Record{ 4 };
         },
         [&creates]
         {
             ++creates;
             return Record{ 5 };
         },
         []( const Record&, ReferenceOrigin ) {} );

    EXPECT_EQ( finds, 1 );
    EXPECT_EQ( creates, 0 );
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// 2 + 3. THE CENSUS. One implementation to be careful in, and every route through it.
//
// The scene parse was not the only place with a find-else-create over an asset reference. Two more files
// had written their own, and each dropped a DIFFERENT part of it: the drag-and-drop import registered
// nothing on either of its reuse paths (under a comment reading "Already cooked + registered?" — a
// question mark where a check belongs), and the thumbnail resolver did the same and then reported the
// consequence with another condition's words.
//
// So the assertion is not "each file is careful". It is that there is ONE implementation to be careful in
// — none of the three touches a service itself — and that inside each file no route escapes it. A rule
// enforced by repetition is what Ф5 paid forty-four call sites for.
// ─────────────────────────────────────────────────────────────────────────────────────────────────

TEST( SceneAssetRegistration, TheThreeServicesAreReachedFromExactlyOnePlace )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository was not found from the test's working directory";

    const std::string registrar = WithoutCommentsAndStrings( ReadAll( root + kRegistrar ) );
    ASSERT_FALSE( registrar.empty() );

    for ( const char* accessor : { "GetMeshService()", "GetMaterialService()", "GetTextureService()" } )
    {
        EXPECT_EQ( ServiceTouches( registrar, accessor ), 1u )
             << accessor << " is reached more than once inside the registrar itself";

        for ( const auto& relative : ResolverFiles() )
        {
            const std::string source = WithoutCommentsAndStrings( ReadAll( root + relative ) );
            ASSERT_FALSE( source.empty() ) << relative;
            EXPECT_EQ( ServiceTouches( source, accessor ), 0u )
                 << relative << " reaches " << accessor << " itself instead of going through " << kRegistrar;
        }
    }

    // MEASURED ON THE TREES THESE TWO TASKS STARTED FROM, and this is the red the suite was shown in:
    // before Ф6, `ComponentRegistry.cpp` reached the mesh service twice and the material service three
    // times (a create-branch registration, a `Register` behind a `Get`-as-a-guard, and the guard itself).
    // Before Ф7, `MeshDnD.cpp` reached them five times and `ThumbnailSubject.cpp` twice, each with its own
    // copy of the rule and each with a different part of it missing.
    //
    // THE TEXTURE COUNT WAS ALREADY 1 IN `ComponentRegistry.cpp` AND THE RELATION WAS STILL BROKEN, which
    // is exactly what a count cannot see: the one site was in `FromPath` and `FromGuid` registered
    // nothing. So the count above is only half the census, and the second half is the test below.
}

// EVERY LOOKUP IS FOLLOWED BY A REGISTRATION — the relation, as an inequality between two counts taken
// from the SAME file. It is what the site count cannot express: a file may touch the registrar once and
// still have a second route that quietly does not.
TEST( SceneAssetRegistration, EveryResolverLooksUpNoMoreOftenThanItRegisters )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const auto& relative : ResolverFiles() )
    {
        const std::string source =
             WithoutTheSerializationDirection( WithoutCommentsAndStrings( ReadAll( root + relative ) ) );
        ASSERT_FALSE( source.empty() ) << relative;

        EXPECT_GE( MeshRegistrations( source ), MeshLookups( source ) )
             << relative << " resolves a mesh reference on a route that registers nothing";
        EXPECT_GE( MaterialRegistrations( source ), MaterialLookups( source ) )
             << relative << " resolves a material reference on a route that registers nothing";
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// 4. Ф7 — A REFUSAL NAMES THE CONDITION THAT HELD.
//
// `ThumbnailSubject::ResolveMesh` had ONE message for THREE facts, and the words it chose belonged to the
// rarest: a mesh nothing had registered was reported as "built no drawable geometry (a skinned mesh's
// static buffer is empty by design)", which sends the reader to look at rigs. The header's own doc block
// promised three distinct refusals; the code had one. DC §1.4 in its soft form — the answer is not empty,
// it is misdirecting, and a misdirecting answer is followed.
//
// The classification is now a pure function of three facts, so the mapping is a thing a test can hold to.
// ─────────────────────────────────────────────────────────────────────────────────────────────────

using Desert::Runtime::ClassifyMeshReadiness;
using Desert::Runtime::ExplainMeshReadiness;
using Desert::Runtime::MeshReadiness;

TEST( SceneAssetRegistration, EachUndrawableMeshHasItsOwnReason )
{
    EXPECT_EQ( ClassifyMeshReadiness( /*registered=*/false, /*built=*/false, 0 ), MeshReadiness::NotRegistered );
    EXPECT_EQ( ClassifyMeshReadiness( true, /*built=*/false, 0 ), MeshReadiness::NotBuilt );
    EXPECT_EQ( ClassifyMeshReadiness( true, true, /*submeshes=*/0 ), MeshReadiness::NoSubmeshes );
    EXPECT_EQ( ClassifyMeshReadiness( true, true, /*submeshes=*/1 ), MeshReadiness::Drawable );

    // THE ONE THAT WOULD HAVE CAUGHT THE DEFECT. An unregistered mesh is also unbuilt and also has no
    // submeshes, so every fact the old message asserted was *true of it* — and the message was still
    // wrong, because it named the last cause instead of the first. Asserting the ORDER of the three is
    // what makes "names the condition that held" a testable claim rather than a wish.
    EXPECT_EQ( ClassifyMeshReadiness( false, false, 0 ), MeshReadiness::NotRegistered );
    EXPECT_NE( ClassifyMeshReadiness( false, false, 0 ), MeshReadiness::NoSubmeshes );
}

TEST( SceneAssetRegistration, NoTwoReadinessStatesShareASentence )
{
    const std::vector<MeshReadiness> all = { MeshReadiness::Drawable, MeshReadiness::NotRegistered,
                                             MeshReadiness::NotBuilt, MeshReadiness::NoSubmeshes };

    std::vector<std::string> sentences;
    for ( const auto state : all )
    {
        const std::string sentence = ExplainMeshReadiness( state, "Cooked/Meshes/probe.stmesh" );
        // Every refusal carries the file. A message a human cannot search for is a message that costs the
        // next person the same investigation.
        EXPECT_NE( sentence.find( "Cooked/Meshes/probe.stmesh" ), std::string::npos ) << sentence;
        sentences.push_back( sentence );
    }

    for ( size_t i = 0; i < sentences.size(); ++i )
    {
        for ( size_t j = i + 1; j < sentences.size(); ++j )
        {
            EXPECT_NE( sentences[i], sentences[j] )
                 << "two readiness states report themselves with the same words: " << sentences[i];
        }
    }

    // And the sentence that misled: it belongs to NoSubmeshes and to nothing else.
    EXPECT_NE( ExplainMeshReadiness( MeshReadiness::NoSubmeshes, "m" ).find( "skinned mesh" ), std::string::npos );
    EXPECT_EQ( ExplainMeshReadiness( MeshReadiness::NotRegistered, "m" ).find( "skinned mesh" ),
               std::string::npos );
}
