// WHAT WE ACTUALLY GOT — three questions only the built artifact can answer, asked of the library this
// workspace links rather than of the build files that asked for it.
//
// The build files state an intention: a pinned submodule, three formats, single precision. Whether the
// binary agrees is a different fact, and it is the one that decides what a user's import does. Every
// assertion here therefore reads the LIBRARY: aiGetImportFormatDescription for what it can parse,
// aiGetVersion* for which assimp it is, and a real file on disk for what an import produces.
//
// WHY THE NUMBERS ARE THE POINT AND NOT "IT DID NOT CRASH". This subsystem has already shipped an empty
// success: `MeshService::Get` handed out a cooked mesh with ZERO submeshes while reporting success, and
// the frame was a picture of clean sky that counted itself as captured (Г15/Д15). A file that parses to
// nothing parses perfectly. So the corpus below is asserted in counts — meshes, vertices, triangles,
// bones, animations — against numbers read off a run, and the two probes are DIFFERENT SHAPES so that
// "the importer returned the same thing for everything" cannot pass.

#include <gtest/gtest.h>

#include <assimp/Importer.hpp>
#include <assimp/cimport.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/version.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::filesystem::path RepositoryRoot()
    {
        std::filesystem::path here = std::filesystem::current_path();
        for ( int up = 0; up < 8 && !std::filesystem::exists( here / "BuildScripts" / "ThirdParty" ); ++up )
        {
            here = here.parent_path();
        }
        return here;
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  out;
        out << in.rdbuf();
        return out.str();
    }

    // THE ENGINE'S OWN POST-PROCESS SET, and the test below asserts that it is still the engine's. Every
    // count in this file depends on these flags: aiProcess_JoinIdenticalVertices alone turns the rigged
    // probe's 8 authored vertices into 6. Numbers measured under one flag set and read under another are
    // the kind of evidence that looks like evidence.
    constexpr unsigned kEngineImportFlags = aiProcess_Triangulate | aiProcess_GenNormals |
                                            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices |
                                            aiProcess_LimitBoneWeights | aiProcess_GlobalScale;

    struct Counts
    {
        unsigned Meshes     = 0;
        unsigned Vertices   = 0;
        unsigned Triangles  = 0;
        unsigned Bones      = 0;
        unsigned Animations = 0;
        unsigned Materials  = 0;
    };

    Counts ImportCounts( const std::filesystem::path& file, std::string& error )
    {
        Assimp::Importer importer;
        const aiScene*   scene = importer.ReadFile( file.string(), kEngineImportFlags );
        Counts           counts;
        if ( scene == nullptr )
        {
            error = importer.GetErrorString();
            return counts;
        }
        counts.Meshes     = scene->mNumMeshes;
        counts.Animations = scene->mNumAnimations;
        counts.Materials  = scene->mNumMaterials;
        for ( unsigned m = 0; m < scene->mNumMeshes; ++m )
        {
            counts.Vertices += scene->mMeshes[m]->mNumVertices;
            counts.Triangles += scene->mMeshes[m]->mNumFaces;
            counts.Bones += scene->mMeshes[m]->mNumBones;
        }
        return counts;
    }

    // The register's extension column, as a set. Rows are `NAME  ext [ext ...]  reason...`; the
    // extensions are the words before the first word that starts with a capital letter, which is where
    // the prose begins. Deliberately simple: the register is a file people edit by hand, and a parser
    // with rules nobody can see is a second way for it to lie.
    std::set<std::string> RegisteredExtensions()
    {
        std::set<std::string> extensions;
        const std::string     text =
             ReadFile( RepositoryRoot() / "BuildScripts" / "ThirdParty" / "AssimpImporters.txt" );
        std::istringstream lines( text );
        std::string        line;
        while ( std::getline( lines, line ) )
        {
            if ( line.empty() || line[0] == '#' )
            {
                continue;
            }
            std::istringstream fields( line );
            std::string        word;
            fields >> word; // the NAME column
            while ( fields >> word )
            {
                const bool looksLikeProse =
                     word.empty() || std::isupper( static_cast<unsigned char>( word[0] ) ) != 0;
                if ( looksLikeProse )
                {
                    break;
                }
                extensions.insert( word );
            }
        }
        return extensions;
    }

    std::set<std::string> LibraryExtensions()
    {
        std::set<std::string> extensions;
        const unsigned        formats = aiGetImportFormatCount();
        for ( unsigned i = 0; i < formats; ++i )
        {
            const aiImporterDesc* desc = aiGetImportFormatDescription( i );
            if ( desc == nullptr || desc->mFileExtensions == nullptr )
            {
                continue;
            }
            std::istringstream words( desc->mFileExtensions );
            std::string        extension;
            while ( words >> extension )
            {
                extensions.insert( extension );
            }
        }
        return extensions;
    }
} // namespace

// П2. THE REGISTER AND THE LIBRARY DESCRIBE THE SAME SET OF FORMATS.
//
// Asserted as a SET OF NAMES, never as a count. A gate that pins "assimp offers N formats" is satisfied
// by editing N — which is how a register decays into a number somebody adjusts until the build is green.
// The two sides here are genuinely independent: the register is what a person wrote down with a reason,
// and aiGetImportFormatDescription is what the compiled binary will actually try to parse.
TEST( AssimpLibraryPin, TheLibraryParsesExactlyTheFormatsTheRegisterNames )
{
    const std::set<std::string> registered = RegisteredExtensions();
    const std::set<std::string> available  = LibraryExtensions();

    ASSERT_FALSE( registered.empty() )
         << "BuildScripts/ThirdParty/AssimpImporters.txt yielded no extensions, so this comparison had "
            "nothing on one side and would have passed on an empty set.";
    ASSERT_FALSE( available.empty() )
         << "the linked assimp reports NO importers at all. That is a library that parses nothing while "
            "linking perfectly — an empty success in the most literal form.";

    std::vector<std::string> shippedButUnregistered;
    std::set_difference( available.begin(), available.end(), registered.begin(), registered.end(),
                         std::back_inserter( shippedButUnregistered ) );
    std::vector<std::string> registeredButAbsent;
    std::set_difference( registered.begin(), registered.end(), available.begin(), available.end(),
                         std::back_inserter( registeredButAbsent ) );

    const auto join = []( const std::vector<std::string>& items )
    {
        std::string out;
        for ( const auto& item : items )
        {
            out += out.empty() ? "" : ", ";
            out += item;
        }
        return out.empty() ? std::string( "none" ) : out;
    };

    EXPECT_TRUE( shippedButUnregistered.empty() )
         << "the built library parses formats the register does not name: [" << join( shippedButUnregistered )
         << "]. Every format is a parser inside the editor's address space, so each one owes the register "
            "a row and a reason — add the row, do not widen the test.";
    EXPECT_TRUE( registeredButAbsent.empty() )
         << "the register names formats the built library CANNOT parse: [" << join( registeredButAbsent )
         << "]. An importer that is disabled but written down is worse than one that is simply off: the "
            "file says the engine accepts a format it will refuse.";
}

// П3. ONE VERSION, AND IT IS OBSERVABLE. Until D40 nothing anywhere pinned an assimp version — Windows
// linked a July binary, macOS linked Homebrew's — which is the single reason two machines could build a
// different engine from one commit. The pin lives in the submodule; this asserts that what got LINKED
// is what the pin says, by comparing the running library against the version Assimp.lua read out of the
// submodule's own PROJECT() line and wrote down.
TEST( AssimpLibraryPin, TheLinkedLibraryIsTheVersionThePinnedSubmoduleDeclares )
{
    const std::filesystem::path recorded = RepositoryRoot() / "build" / "generated" / "assimp" / "VERSION.txt";
    ASSERT_TRUE( std::filesystem::exists( recorded ) )
         << "build/generated/assimp/VERSION.txt is missing. Assimp.lua writes it while generating "
            "config.h and revision.h, so its absence means the project was not generated — run "
            "`CI=true premake5 gmake`. Passing without it would be a version census that read no version.";

    std::string text = ReadFile( recorded );
    while ( !text.empty() && ( text.back() == '\n' || text.back() == '\r' ) )
    {
        text.pop_back();
    }

    const std::string linked = std::to_string( aiGetVersionMajor() ) + "." +
                               std::to_string( aiGetVersionMinor() ) + "." + std::to_string( aiGetVersionPatch() );

    EXPECT_EQ( text, linked )
         << "the linked assimp reports " << linked << " while the pinned submodule declares " << text
         << ". These come from different places on purpose: the right-hand side is read from "
            "Editor/ThirdParty/assimp/CMakeLists.txt at generation time, the left-hand side from the "
            "binary that will actually parse a user's file. A mismatch means something other than the "
            "submodule was linked — a system library, or a stale archive.";

    // Single precision, which every engine struct that copies an aiVector3D assumes. Asserted through
    // the type rather than through a macro, because the macro is what a build file sets and the type is
    // what a memcpy sees.
    EXPECT_EQ( sizeof( ai_real ), sizeof( float ) )
         << "assimp was built with ASSIMP_DOUBLE_PRECISION. Every conversion in AssimpImporter reads "
            "aiVector3D as three floats; at double precision it silently reads half a vector.";
}

// П4. A REAL FILE IMPORTS TO EXACT NUMBERS, and the numbers were read off a run, never predicted.
//
// TWO PROBES, DIFFERENT SHAPES. `base.fbx` is the project's real 4.5 MB static model; `TwoJointProbe.gltf`
// is a hand-authored rigged one, and it exists because NO committed model had a bone or an animation in
// it — so the skinned half of the importer, and the glTF row of the register, were both unfalsifiable.
// Their counts differ in every column, which is what makes "the importer answers the same for
// everything" a failing answer rather than a passing one.
TEST( AssimpLibraryPin, TheCommittedModelsImportToTheGeometryTheyCarry )
{
    const auto root = RepositoryRoot();

    // The static model. The same 1/105 317/120 000 that Cooked/Meshes/base.stmesh carries, which is the
    // relation worth having: the cooked artifact and the source file agree, so the cook is faithful.
    {
        std::string  error;
        const Counts counts = ImportCounts( root / "Editor/Resources/Assets/Meshes/base.fbx", error );
        ASSERT_TRUE( error.empty() ) << "base.fbx did not import: " << error;
        EXPECT_EQ( counts.Meshes, 1u );
        EXPECT_EQ( counts.Vertices, 105317u );
        EXPECT_EQ( counts.Triangles, 120000u );
        EXPECT_EQ( counts.Materials, 1u );
        // Static, and asserted rather than assumed: if this file ever grows a rig, the numbers below for
        // the rigged probe stop being the only bone coverage in the tree and this line is where that
        // gets noticed.
        EXPECT_EQ( counts.Bones, 0u );
        EXPECT_EQ( counts.Animations, 0u );
    }

    // The rigged probe. 8 vertices are authored and 6 arrive: aiProcess_JoinIdenticalVertices merges the
    // seam where the two quads meet. That is not a wart, it is the evidence that the engine's flag set
    // is actually applied — a run with the flags dropped reports 8.
    {
        std::string  error;
        const Counts counts = ImportCounts( root / "Editor/Resources/Assets/Meshes/TwoJointProbe.gltf", error );
        ASSERT_TRUE( error.empty() ) << "TwoJointProbe.gltf did not import: " << error
                                     << " — if this says the format is unknown, the GLTF row of "
                                        "AssimpImporters.txt is no longer taking effect.";
        EXPECT_EQ( counts.Meshes, 1u );
        EXPECT_EQ( counts.Vertices, 6u );
        EXPECT_EQ( counts.Triangles, 4u );
        EXPECT_EQ( counts.Bones, 2u )
             << "the rigged probe lost its skin. It is the ONLY model in this repository with a bone in "
                "it, so this number is the whole of the skinned import path's coverage.";
        EXPECT_EQ( counts.Animations, 1u )
             << "the rigged probe lost its animation — likewise the only one in the tree.";
    }
}

// THE FLAG SET THE NUMBERS ABOVE WERE MEASURED UNDER IS STILL THE ENGINE'S.
//
// Every count in this file is a function of the six post-process steps AssimpImporter passes. A test
// that hardcoded its own flags would keep passing while the engine moved to a different set, and would
// then be asserting numbers nobody's import produces. So the flags are read out of the importer's
// source and compared against the ones used here, name by name.
TEST( AssimpLibraryPin, TheImporterStillUsesTheFlagsTheseNumbersWereMeasuredUnder )
{
    const std::string source =
         ReadFile( RepositoryRoot() / "Editor/Source/Editor/Import/Assimp/AssimpImporter.cpp" );
    ASSERT_FALSE( source.empty() ) << "AssimpImporter.cpp could not be read, so this comparison had "
                                      "nothing to compare against.";

    const std::vector<std::string> expected = { "aiProcess_Triangulate",      "aiProcess_GenNormals",
                                                "aiProcess_CalcTangentSpace", "aiProcess_JoinIdenticalVertices",
                                                "aiProcess_LimitBoneWeights", "aiProcess_GlobalScale" };
    for ( const auto& flag : expected )
    {
        EXPECT_NE( source.find( flag ), std::string::npos )
             << "AssimpImporter.cpp no longer passes " << flag
             << ", so the counts this suite asserts were measured under a flag set the engine has "
                "stopped using. Re-measure them from a run and update both together.";
    }

    // And no SEVENTH flag: an added step changes vertex counts (JoinIdenticalVertices already turns 8
    // into 6), so a new one has to arrive with re-measured numbers rather than quietly.
    std::size_t found = 0;
    for ( std::size_t at = source.find( "aiProcess_" ); at != std::string::npos;
          at             = source.find( "aiProcess_", at + 1 ) )
    {
        ++found;
    }
    EXPECT_EQ( found, expected.size() )
         << "AssimpImporter.cpp names " << found << " aiProcess_ flags where this suite knows " << expected.size()
         << ". A post-process step is a transformation of the geometry, so the numbers above must be "
            "re-measured in the same change that adds or removes one.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
