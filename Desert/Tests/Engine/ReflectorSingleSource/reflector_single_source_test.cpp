// "`rfl::Reflector` IS SPECIALISED IN EXACTLY ONE HEADER OF THIS TREE" — AS A RELATION, NOT AS A
// DELETION.
//
// WHAT WAS MEASURED, ON `dev` AT b871fe59, BEFORE ANY CHANGE. Three headers specialised `rfl::Reflector`
// for types this engine owns, and they overlapped:
//
//   Desert/Common/Source/Common/Core/Serialization/GlmReflection.hpp
//        AABB, UUID, AssetHandle, mat4, vec2, vec3, vec4, quat
//   Desert/Desert/Source/Engine/Core/Serialize/GLMReflect.hpp        vec2, vec3, vec4
//   Desert/Desert/Source/Engine/Core/Serialize/CustomReflect.hpp     UUID, AssetHandle
//
// The second and third always travelled together — the same thirteen translation units included the
// pair — and both are proper subsets of the first, with the identical wire form (a bare `uint64` for a
// handle, a `std::array<float,N>` for a vector). A13 deleted both and repointed those thirteen.
//
// WHY THIS IS NOT A TIDINESS QUESTION. Two specialisations of one template for one type in two headers
// cannot both be included in one translation unit: the second is a redefinition error. So the two halves
// of the tree could never meet, and the header a developer happened to reach for silently decided which
// types they were allowed to serialise — the deleted `GLMReflect.hpp` could not spell a `glm::quat`,
// which is every rotation this engine stores. Nothing reported that. It was found by accident, by A12,
// while doing something else. And the compiler cannot see it until somebody writes the include that
// breaks: the error then lands in an innocent file, naming two headers neither of which is at fault.
//
// A DELETION IS NOT THE DELIVERABLE, because nothing stops a fourth header tomorrow. This census
// collects every file in the tree that specialises `rfl::Reflector` and asserts the SET is the one file
// named below — the count is DERIVED from what the walk finds, never pinned as a literal that a future
// edit could satisfy by changing the literal.
//
// AND THE SECOND RULE IS THE OTHER DIRECTION. "Exactly one header" is also satisfied by a header that
// has been emptied out, which is precisely the state the deleted `GLMReflect.hpp` was in. So the
// surviving header is held to one NAMED ROW PER TYPE the tree serialises, with that count derived from
// the rows too.
//
// Comments and string literals are blanked with the shared reader (Д33) before anything is matched.
// This file names every symbol it is about, and a census that counted its own prose would be worthless
// — it would find a specialisation in itself and report the defect it was written to prevent.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    /// The header every other file must reach for. One constant, so the two tests cannot disagree about
    /// which one survived.
    const char* const kTheOneHeader = "Desert/Common/Source/Common/Core/Serialization/GlmReflection.hpp";

    /// The sentinel is the header this census is ABOUT, so a run from the wrong directory fails as "could
    /// not find the root" rather than as "nobody specialises anything" — which is the shape of a census
    /// that certifies an empty scan.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + kTheOneHeader );
            if ( probe )
            {
                return prefix;
            }
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    /// The trees this engine writes. ThirdParty is excluded because reflect-cpp's own headers specialise
    /// `Reflector` legitimately, and vendored code is not ours to hold to this rule.
    const std::vector<std::string>& OurSourceRoots()
    {
        static const std::vector<std::string> roots = { "Desert/Common/Source", "Desert/Desert/Source",
                                                        "Desert/Tests",         "Editor/Source",
                                                        "Runtime/Source",       "Tools" };
        return roots;
    }

    bool IsSource( const fs::path& path )
    {
        const std::string ext = path.extension().string();
        return ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".inl";
    }

    /**
     * @brief Does @p code DEFINE a `Reflector` specialisation?
     *
     * Matches the definition's head — `struct Reflector<` and the redundantly-qualified
     * `struct rfl::Reflector<` this tree also contains — and not the word "Reflector". A `Reflector<T>`
     * merely USED, by reflect-cpp's machinery or by a `static_assert`, creates no second source of truth;
     * counting uses would paint every file that serialises a vec3 red, which is most of them.
     */
    bool DefinesASpecialisation( const std::string& code )
    {
        return code.find( "struct Reflector<" ) != std::string::npos ||
               code.find( "struct rfl::Reflector<" ) != std::string::npos;
    }
} // namespace

TEST( ReflectorSingleSourceTest, ExactlyOneHeaderOfThisTreeSpecialisesRflReflector )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    std::vector<std::string> owners;
    std::size_t              scanned = 0;

    for ( const std::string& subtree : OurSourceRoots() )
    {
        const fs::path base = root + subtree;
        ASSERT_TRUE( fs::exists( base ) ) << "source root " << subtree
                                          << " is not there; the scan would be "
                                             "silently empty, which is the shape of a census that certifies "
                                             "nothing";
        for ( const auto& entry : fs::recursive_directory_iterator( base ) )
        {
            if ( !entry.is_regular_file() || !IsSource( entry.path() ) )
            {
                continue;
            }
            // ThirdParty is vendored inside some of these trees.
            if ( entry.path().string().find( "ThirdParty" ) != std::string::npos )
            {
                continue;
            }

            ++scanned;
            const std::string code =
                 Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( entry.path() ) );
            if ( !DefinesASpecialisation( code ) )
            {
                continue;
            }

            std::string relative = entry.path().string();
            if ( relative.starts_with( root ) )
            {
                relative = relative.substr( root.size() );
            }
            // WINDOWS. `fs::path::string()` hands back this platform's separator, so the rows below —
            // and every message this census prints — would be `\`-spelled there and would match
            // nothing. Normalised to `/` so the expected set is ONE literal for both platforms rather
            // than a second one nobody on this machine can test. (Seven defects of the
            // "compiles here, not there" class have reached `dev`; this is the cheap end of it.)
            std::replace( relative.begin(), relative.end(), '\\', '/' );
            owners.push_back( relative );
        }
    }

    // THE WALK MUST HAVE SEEN THE TREE. A census whose scan found nothing reports "exactly zero extra
    // headers" in the same confident voice it uses when that is true.
    EXPECT_GT( scanned, 1000U ) << "only " << scanned << " source files were read; the walk is not working";

    std::sort( owners.begin(), owners.end() );
    const std::vector<std::string> expected = { kTheOneHeader };
    EXPECT_EQ( owners, expected )
         << "an rfl::Reflector specialisation exists outside " << kTheOneHeader
         << ". Two headers specialising one template for one type cannot both be included in one "
            "translation unit, so this is not a duplication of style — it is two halves of the tree that "
            "can never meet, and the compiler says nothing until somebody writes the include that breaks.";

    // The count is DERIVED from what the walk found, never pinned as a literal: a gate pinning a number
    // can be satisfied by editing the number.
    EXPECT_EQ( owners.size(), expected.size() );
}

TEST( ReflectorSingleSourceTest, TheSurvivingHeaderStillSpellsEveryTypeTheTreeSerialises )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string code =
         Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( root + kTheOneHeader ) );
    ASSERT_FALSE( code.empty() );

    // ONE NAMED ROW PER TYPE, the count derived from the rows. "Exactly one header" is otherwise
    // satisfiable by a header that has been emptied out — which is exactly the state the deleted
    // `GLMReflect.hpp` was in, and it still looked like a complete answer.
    struct Row
    {
        const char* Type;
        const char* Why;
    };
    const std::vector<Row> rows = {
         { "glm::vec2", "UI sizes and texture coordinates" },
         { "glm::vec3", "every translation, scale and colour in the scene format" },
         { "glm::vec4", "material parameters and tints" },
         { "glm::quat", "every serialised ROTATION. The header A13 deleted could not spell one, which is "
                        "why it was the one deleted and not the survivor" },
         { "glm::mat4", "baked transforms — bone offsets and instance transforms" },
         { "Common::UUID", "entity identity in every scene and prefab" },
         { "Common::AssetHandle", "every asset slot; it is a DISTINCT type from UUID and needs its own row" },
         { "Common::Math::AABB", "mesh and instance bounds" },
    };

    std::size_t covered = 0;
    for ( const Row& row : rows )
    {
        SCOPED_TRACE( row.Type );
        // The tree spells one of these with a leading `::` and one with the enclosing namespace opened,
        // so both are accepted: a rename of the spelling is not a loss of the reflector.
        const std::string type( row.Type );
        const bool        found = code.find( "struct Reflector<" + type + ">" ) != std::string::npos ||
                           code.find( "struct rfl::Reflector<" + type + ">" ) != std::string::npos ||
                           code.find( "struct Reflector<::" + type + ">" ) != std::string::npos;
        EXPECT_TRUE( found ) << "nothing serialises " << row.Type << " any more: " << row.Why;
        if ( found )
        {
            ++covered;
        }
    }
    EXPECT_EQ( covered, rows.size() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
