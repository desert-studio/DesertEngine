// "THE glm REFLECTORS LIVE IN EXACTLY ONE HEADER" — AS A RELATION OVER THE TREE, NOT AS A DELETION.
//
// The tree carried TWO headers specialising `rfl::Reflector` for `glm::vec2/3/4`:
// `Common/Core/Serialization/GlmReflection.hpp` (which also has `quat` and `mat4`) and
// `Engine/Core/Serialize/GLMReflect.hpp` (vectors only), with thirteen translation units including the
// second. Two specialisations of one template for one type in two headers is not a tidiness question —
// a translation unit that includes both is ill-formed, so the two halves of the tree could never meet,
// and which header a developer reached for silently decided whether they were allowed to serialise a
// rotation. A13 deleted the smaller one and repointed its includers.
//
// A DELETION IS NOT THE DELIVERABLE. The project's own rule: turn a one-off fix into an assertion, or
// the second source of truth grows back — nothing in a compiler or a review stops somebody adding a
// third header tomorrow, and the first symptom would again be an include that fails for a reason that
// reads like something else.
//
// SO THIS CENSUS ASSERTS THE RELATION AND DERIVES ITS NUMBERS. It never pins "there is 1 header" as a
// literal that a future edit could satisfy by changing the literal; it collects every file in the tree
// that specialises `rfl::Reflector` for a glm type and asserts the SET is the one file named below.
// The second rule is the other direction: that surviving header must still cover every glm type the
// tree serialises, one named row per type, with the count derived from the rows — otherwise "exactly
// one header" could be satisfied by a header that has been emptied out.
//
// Comments and string literals are blanked with the shared reader (Д33) before anything is matched:
// this file names the symbols it is about, and a census that counted its own prose would be worthless.

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

    /// The sentinel is the header this census is ABOUT, so a run from the wrong directory fails as "could
    /// not find the root" rather than as "nobody specialises anything", which is the shape of a census
    /// that certifies an empty scan.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Common/Source/Common/Core/Serialization/GlmReflection.hpp" );
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

    /// The trees this engine writes. ThirdParty is excluded because reflect-cpp's own headers legitimately
    /// specialise `Reflector`, and vendored code is not ours to hold to this rule.
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
     * @brief Does @p code specialise `rfl::Reflector` for a glm type, and where.
     *
     * Matches the DECLARATION rather than the word "Reflector": `struct Reflector<glm::` and the
     * redundantly-qualified `struct rfl::Reflector<glm::` that this tree also contains. A `Reflector<T>`
     * merely USED (by reflect-cpp's own machinery, or named in a static_assert) is not a definition and
     * does not create a second source of truth, so it must not be counted — counting uses would make the
     * census red for every file that serialises a vec3, which is most of them.
     */
    std::vector<std::size_t> SpecialisationLines( const std::string& code )
    {
        std::vector<std::size_t>       hits;
        const std::vector<std::string> forms = { "struct Reflector<glm::", "struct rfl::Reflector<glm::" };
        for ( const std::string& form : forms )
        {
            std::size_t at = 0;
            while ( ( at = code.find( form, at ) ) != std::string::npos )
            {
                hits.push_back( 1 + static_cast<std::size_t>(
                                         std::count( code.begin(), code.begin() + static_cast<long>( at ), '\n' ) ) );
                at += form.size();
            }
        }
        std::sort( hits.begin(), hits.end() );
        return hits;
    }
} // namespace

/// The header every other file must reach for. Named once, as a constant, so the two tests below cannot
/// disagree about which one survived.
static const char* const kTheOneHeader = "Desert/Common/Source/Common/Core/Serialization/GlmReflection.hpp";

TEST( GlmReflectorCensusTest, ExactlyOneHeaderOfThisTreeSpecialisesReflectorForAglmType )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    std::vector<std::string> owners;
    std::size_t              scanned = 0;

    for ( const std::string& subtree : OurSourceRoots() )
    {
        const fs::path base = root + subtree;
        ASSERT_TRUE( fs::exists( base ) ) << "source root " << subtree << " is not there; the scan would be "
                                             "silently empty, which is the shape of a census that certifies "
                                             "nothing";
        for ( const auto& entry : fs::recursive_directory_iterator( base ) )
        {
            if ( !entry.is_regular_file() || !IsSource( entry.path() ) )
                continue;
            // ThirdParty is vendored into some of these trees; reflect-cpp specialises its own template
            // legitimately and is not ours to hold to this rule.
            if ( entry.path().string().find( "ThirdParty" ) != std::string::npos )
                continue;

            ++scanned;
            const std::string code = Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( entry.path() ) );
            if ( SpecialisationLines( code ).empty() )
                continue;

            std::string relative = entry.path().string();
            if ( relative.rfind( root, 0 ) == 0 )
                relative = relative.substr( root.size() );
            owners.push_back( relative );
        }
    }

    // THE SCAN MUST HAVE SEEN THE TREE. A census whose walk found nothing reports "exactly zero extra
    // headers" in the same confident voice it uses when that is true.
    EXPECT_GT( scanned, 1000U ) << "only " << scanned << " source files were read; the walk is not working";

    std::sort( owners.begin(), owners.end() );
    const std::vector<std::string> expected = { kTheOneHeader };
    EXPECT_EQ( owners, expected ) << "a glm Reflector specialisation exists outside " << kTheOneHeader
                                  << ". Two headers specialising one template for one type cannot both be "
                                     "included in a translation unit, so this is not a duplication of "
                                     "style — it is two halves of the tree that can never meet.";

    // The count is DERIVED from what the walk found, never pinned as a literal: a gate pinning a number
    // can be satisfied by editing the number.
    EXPECT_EQ( owners.size(), expected.size() );
}

TEST( GlmReflectorCensusTest, TheSurvivingHeaderStillSpellsEveryGlmTypeTheTreeSerialises )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string code =
         Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( root + kTheOneHeader ) );
    ASSERT_FALSE( code.empty() );

    // ONE NAMED ROW PER TYPE, and the count derived from the rows. "Exactly one header" would otherwise be
    // satisfiable by a header that had been emptied out — which is precisely how the deleted one came to
    // be missing `quat` and `mat4` while still looking like a complete answer.
    struct Row
    {
        const char* Type;
        const char* Why;
    };
    const std::vector<Row> rows = {
         { "glm::vec2", "UI sizes and texture coordinates" },
         { "glm::vec3", "every translation, scale and colour in the scene format" },
         { "glm::vec4", "material parameters and tints" },
         { "glm::quat", "every serialised ROTATION; the header deleted by A13 could not spell one, which "
                        "is why it was the one deleted" },
         { "glm::mat4", "baked transforms — bone offsets and instance transforms" },
    };

    std::size_t covered = 0;
    for ( const Row& row : rows )
    {
        SCOPED_TRACE( row.Type );
        const bool plain     = code.find( std::string( "struct Reflector<" ) + row.Type + ">" ) != std::string::npos;
        const bool qualified = code.find( std::string( "struct rfl::Reflector<" ) + row.Type + ">" ) !=
                               std::string::npos;
        EXPECT_TRUE( plain || qualified ) << "nothing serialises " << row.Type << " any more: " << row.Why;
        if ( plain || qualified )
            ++covered;
    }
    EXPECT_EQ( covered, rows.size() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
