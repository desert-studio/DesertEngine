// HOW MANY GPU PROGRAMS JUDGE THE CLOUD MEDIUM, and the census that makes the number impossible to grow
// by accident.
//
// WHY THIS SUITE EXISTS, AND IT IS THE COST INPUT OF O1-C. The O1 design (Docs/Clouds/O1_DESIGN.md §4)
// names the seam an authored medium has to enter through as TWO sites: the view march's call to
// CloudSampleDensity and the light march inside it. The tree has FOUR shipped GPU programs that sample the
// cloud field, and three of them are not the view march. An authored medium that reached only the march
// would leave the other three judging the same sky by a different field — the shape each of those files
// warns about IN ITS OWN HEADER:
//
//   * Programs/Clouds/CloudShadowMap.shader — "a cirrus eroded by the fine volume for the eye and by the
//     default one for the shadow map would be two different clouds in one frame";
//   * Programs/Clouds/CloudSkyOcclusionVolume.shader — "a column integrated from a different field than
//     the one the eye marches would darken clouds that are not there";
//   * Programs/Compute/BakeProceduralSky.shader — "not by an analytic dome standing beside them, which
//     would be a SECOND model of the clouds and therefore a mirror that drifts. This project has paid for
//     that shape once already, in the grey-clouds defect".
//
// AND IT IS MEASURED, not argued. Zeroing the medium at ONE consumer's own call site and shooting
// Clouds_Protocol at 90 frames, 1280-wide, against a noise floor measured at 0 of 560 560 differing
// pixels on every point (2026-09-08, Debug/MoltenVK):
//
//   consumer                    zenith            mid              horizon
//   cloud shadow map            0.000 %           0.000 %          29.978 %, max 146/255
//   sky-light occlusion volume  89.411 %, max 32  88.273 %, max 32 99.117 %, max 29
//   sky panorama bake (IBL)     0.000 %           0.000 %          29.799 %, max  44
//
// So every one of the three is LIVE, the occlusion volume touches nine tenths of every frame, and two of
// them are invisible from the zenith and the mid angle — which is exactly how a divergence introduced here
// would pass a review that shot only upward.
//
// WHAT IS ASSERTED. The set of programs that sample the field, BY NAME, and that each of them takes its
// density from the shared CloudSampleDensity rather than deriving one. A fifth consumer is red, and it is
// red on purpose: whoever adds it has to decide what it does about an authored medium, in the same change.
// The count is quoted so a census that silently shrank is visible too.
//
// Pure: reads the shader tree as text. It IS a text census, and that is what a census of FILES has to be —
// the subject is which files exist, which no C++ symbol can answer.

#include <Engine/Graphic/Clouds/CloudPayload.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    std::filesystem::path RepoRoot()
    {
        std::filesystem::path prefix = ".";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::filesystem::exists( prefix / "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" ) )
                return prefix;
            prefix /= "..";
        }
        return {};
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Every file under the shader tree with the given extension, as a path relative to the shader root, so
    // a message names the file the way a person would type it.
    std::set<std::string> ShaderFiles( const std::filesystem::path& root, const std::string& extension )
    {
        std::set<std::string> found;
        if ( !std::filesystem::exists( root ) )
            return found;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
        {
            if ( entry.is_regular_file() && entry.path().extension() == extension )
                found.insert( std::filesystem::relative( entry.path(), root ).generic_string() );
        }
        return found;
    }
} // namespace

// ── THE CENSUS ─────────────────────────────────────────────────────────────────────────────────────────
TEST( CloudMediumConsumers, EveryProgramThatSamplesTheCloudFieldIsOneOfTheFourKnownOnes )
{
    const std::filesystem::path shaders  = RepoRoot() / "Engine/Content/Shaders";
    const std::set<std::string> programs = ShaderFiles( shaders / "Programs", ".shader" );
    ASSERT_FALSE( programs.empty() ) << "the shader tree was not found, so this census counted nothing";

    std::set<std::string> consumers;
    std::set<std::string> deriveTheirOwn;

    for ( const std::string& relative : programs )
    {
        const std::string source = ReadAll( shaders / "Programs" / relative );
        if ( source.find( "SampleCloudField(" ) == std::string::npos )
            continue;

        consumers.insert( relative );

        // TAKING THE DENSITY FROM THE SHARED FUNCTION IS THE WHOLE POINT OF THE SEAM. A program that has
        // the field sample and computes a density of its own is a second model of the medium, which is the
        // defect BakeProceduralSky's own header describes having been paid for once.
        if ( source.find( "CloudSampleDensity(" ) == std::string::npos )
            deriveTheirOwn.insert( relative );
    }

    // THE FOUR, NAMED, with what each of them is for — a count alone would pass on the wrong four the day
    // one is renamed, and the names are what a reader of a red message needs.
    const std::set<std::string> expected = {
         "Clouds/CloudRaymarch.shader",           // the view march: what the camera sees
         "Clouds/CloudShadowMap.shader",          // the clouds' shadow on the ground
         "Clouds/CloudSkyOcclusionVolume.shader", // how much sky each column can see
         "Compute/BakeProceduralSky.shader",      // the panorama the scene is lit from (IBL)
    };

    EXPECT_EQ( consumers, expected )
         << "the set of GPU programs that sample the cloud field is not the four this tree knows about. A "
            "program ADDED here has to answer the question O1-C exists to answer — where does its medium "
            "come from once a material authors one — and a program REMOVED here means one of the four "
            "stopped judging the sky by the shared field, which is a change of what the frame shows.";

    EXPECT_TRUE( deriveTheirOwn.empty() )
         << deriveTheirOwn.size()
         << " program(s) sample the cloud field and do NOT call CloudSampleDensity, so they carry a second "
            "model of the medium. The first of them is '"
         << ( deriveTheirOwn.empty() ? std::string( "-" ) : *deriveTheirOwn.begin() ) << "'.";

    std::printf( "[CloudMediumConsumers] %zu of %zu shipped programs sample the cloud medium\n", consumers.size(),
                 programs.size() );
}

// ── THE SEAM IS ONE TEXT, AND IT IS THE SUBSTITUTABLE ONE ──────────────────────────────────────────────
//
// The four consumers above reach the medium through ONE definition, and the second march inside
// CloudField.glslh (the sun-transmittance quadrature) reaches it there too. Since O1-E that definition
// lives in Generated/CloudMedium.glslh, which is the file a cloud material SUBSTITUTES at compile time
// (Engine/Core/ShaderCompiler/ShaderVariant.hpp). Both halves are asserted, because they fail
// differently:
//
//   * a SECOND definition of any medium entry point is a mirror — the four programs would then judge the
//     sky by whichever of them they happened to include, which is the grey-clouds shape;
//   * a definition that is not in the substitutable file is a medium NO material can author, and the
//     symptom is a graph that compiles, applies, and changes nothing.
//
// EVERY ENTRY POINT OF THE CONTRACT IS CHECKED, not just density. Extinction, albedo, emission and
// occlusion are outputs of the Volume domain's contract (O1_DESIGN §3.3) and each is a place a mirror
// could grow.
TEST( CloudMediumConsumers, EveryMediumEntryPointIsDefinedOnceAndInTheSubstitutableFile )
{
    const std::filesystem::path shaders = RepoRoot() / "Engine/Content/Shaders";
    const std::set<std::string> headers = ShaderFiles( shaders, ".glslh" );
    ASSERT_FALSE( headers.empty() ) << "the shared shader headers were not found";

    // The name the four programs' include closure resolves, and the name a material substitutes. One
    // string, used for both, so the test cannot pass while the two have drifted apart.
    const std::string kMediumInclude = "Generated/CloudMedium.glslh";

    // The file that is NEVER substituted, and therefore the one an authored medium may still call.
    const std::string kMediumDefaults = "Common/CloudMediumDefault.glslh";

    const std::vector<std::pair<std::string, std::string>> entryPoints = {
         { "float CloudSampleDensity(", kMediumInclude },
         { "float CloudSampleExtinctionFactor(", kMediumInclude },
         { "vec3 CloudSampleAlbedo(", kMediumInclude },
         { "vec3 CloudSampleEmissive(", kMediumInclude },
         { "float CloudSampleOcclusion(", kMediumInclude },
         // The shipped bodies. They live in the un-substituted half on purpose: a graph that says "the
         // same clouds, but…" calls them, and the whole mechanism's acceptance — a graph that changes
         // nothing produces the frame that no graph produces — rests on them being reachable.
         { "float CloudDefaultDensity(", kMediumDefaults },
         { "float CloudDefaultExtinctionFactor(", kMediumDefaults },
         { "vec3 CloudDefaultAlbedo(", kMediumDefaults },
         { "vec3 CloudDefaultEmissive(", kMediumDefaults },
         { "float CloudDefaultOcclusion(", kMediumDefaults },
         // What the graph's Cloud Sample node compiles to. Its pin NAMES are this struct's member names,
         // which is what saves the emitter a pin-to-expression table that would have to be kept level
         // with the node catalogue by hand.
         { "struct CloudGraphSample", kMediumDefaults },
         { "CloudGraphSample CloudGraphSampleAt(", kMediumDefaults },
         // WHAT THE Cloud Noise Volume NODE COMPILES TO CALLS. Its unwired coordinate is the shipped
         // erosion's own texture position and its wired slot goes through the conversion, so both have to
         // live where an authored medium can still reach them — a copy of either in the substituted half
         // is a graph that compiles until somebody authors one, and then does not.
         { "vec3 CloudDefaultWindPositionKm(", kMediumDefaults },
         { "vec3 CloudDefaultNoiseCoordinate(", kMediumDefaults },
         { "int CloudNoiseSlotOf(", kMediumDefaults },
    };

    for ( const auto& [signature, home] : entryPoints )
    {
        std::set<std::string> definers;
        for ( const std::string& relative : headers )
        {
            if ( ReadAll( shaders / relative ).find( signature ) != std::string::npos )
                definers.insert( relative );
        }

        EXPECT_EQ( definers, ( std::set<std::string>{ home } ) )
             << "'" << signature << "' is defined in " << definers.size()
             << " header(s). Exactly one, and it has to be " << home << ". The five entry points belong in "
             << kMediumInclude
             << ", which is the file a material substitutes — a definition elsewhere is a medium no "
                "material can author. The shipped bodies belong in "
             << kMediumDefaults
             << ", which is never substituted — a body in the substituted file is a body an authored "
                "medium cannot call. Two definitions of either is a mirror that drifts.";
    }

    // AND IT IS INCLUDED FROM EXACTLY ONE PLACE. The substitution is by NAME: two headers including it
    // would compile two copies under one name, and the include guard would silently give the second one
    // nothing — a difference between two programs that no diagnostic would ever mention.
    std::set<std::string> includers;
    for ( const std::string& relative : headers )
    {
        if ( relative == kMediumInclude )
            continue;
        if ( ReadAll( shaders / relative ).find( "<" + kMediumInclude + ">" ) != std::string::npos )
            includers.insert( relative );
    }
    EXPECT_EQ( includers, ( std::set<std::string>{ "Common/CloudField.glslh" } ) )
         << kMediumInclude << " is included from " << includers.size()
         << " header(s). One is the seam: every program that samples the field gets it through "
            "Common/CloudField.glslh and none of them names it.";
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════
// EVERY SLOT OF THE CLOUD PARAMETER BLOCK IS READ, OR IT HAS A ROW SAYING WHY NOT
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════
//
// The block's stated discipline is that a slot nobody reads is a dead setting wearing a parameter's
// clothes — and the first person to need a new one repurposes it, with no name, no range and no tooltip.
// std430 makes the discipline unreachable the moment a value becomes a three-component COLOUR: the block
// is a grid of vec4s, so it can only grow by four, and turning one float into three asks for two.
//
// SO THE EXCEPTION IS A REGISTER AND NOT A NUMBER, and this is the test that makes the difference matter.
// A count ("two slots are unread") is the number the next author adjusts instead of explaining;
// Graphic::kCloudUnreadSlots is a row per slot with the reason in it, and a THIRD unread slot goes red
// HERE because it has no row. Both directions are asserted: a row that names a slot the shaders do read is
// red too, because a stale exception is how a real dead slot gets to hide behind an old excuse.
//
// AND IT MEASURES THE SHADERS RATHER THAN TRUSTING A LIST. Which components of which member are fetched is
// read out of the shader tree's own text, with comments stripped first — the block's own header discusses
// `u_CloudAlbedo.w` in prose, and a scan that counted prose would certify the very slot it says is unread.
//
// IT ALSO PINS THE ONE RELATION NOTHING ELSE DOES: the GLSL block and Graphic::CloudGpuPayload agree on
// how many floats they carry. The C++ side has static_asserts on every offset; the GLSL side is a
// hand-written mirror with nothing checking it at all, and a member added to one and not the other is a
// frame in which every parameter after it is read from the wrong place.
namespace
{
    // Comments removed, so prose about a slot cannot be mistaken for a fetch of it. Line and block
    // comments only — the shader dialect has no others, and strings do not appear in these files.
    std::string CodeOnly( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        for ( std::size_t i = 0; i < source.size(); )
        {
            if ( source.compare( i, 2, "//" ) == 0 )
            {
                while ( i < source.size() && source[i] != '\n' )
                    ++i;
                continue;
            }
            if ( source.compare( i, 2, "/*" ) == 0 )
            {
                i += 2;
                while ( i + 1 < source.size() && source.compare( i, 2, "*/" ) != 0 )
                    ++i;
                i = std::min( i + 2, source.size() );
                continue;
            }
            out += source[i++];
        }
        return out;
    }

    // One member of the block, as the GLSL declares it.
    struct BlockMember
    {
        std::string Name;
        int         Components = 4; // 4 for vec4, 3 for the trailing vec3
        int         ArrayCount = 1;
    };

    // The block, read out of the file that declares it. A regex would need the same comment stripping and
    // would still have to be told the two shapes that occur, so it is a plain scan.
    std::vector<BlockMember> ParseBlock( const std::string& code )
    {
        std::vector<BlockMember> members;
        const std::size_t        open = code.find( "readonly buffer CloudParamsBuffer" );
        if ( open == std::string::npos )
            return members;

        const std::size_t begin = code.find( '{', open );
        const std::size_t end   = code.find( '}', begin );
        if ( begin == std::string::npos || end == std::string::npos )
            return members;

        std::istringstream lines( code.substr( begin + 1, end - begin - 1 ) );
        std::string        line;
        while ( std::getline( lines, line ) )
        {
            std::istringstream fields( line );
            std::string        type;
            std::string        name;
            if ( !( fields >> type >> name ) )
                continue;
            if ( type != "vec4" && type != "vec3" )
                continue;

            BlockMember member;
            member.Components = type == "vec4" ? 4 : 3;

            // `u_CloudSpeciesEdge[4];` — the count is part of the name as read.
            const std::size_t bracket = name.find( '[' );
            if ( bracket != std::string::npos )
            {
                member.ArrayCount = std::atoi( name.c_str() + bracket + 1 );
                member.Name       = name.substr( 0, bracket );
            }
            else
            {
                member.Name = name.substr( 0, name.find( ';' ) );
            }
            if ( !member.Name.empty() && member.ArrayCount > 0 )
                members.push_back( member );
        }
        return members;
    }

    // THE BLOCK'S OWN DECLARATION IS NOT A READ, and leaving it in was this census's own blind spot: a
    // member declared as `vec4 u_CloudDetail;` is a bare occurrence of the name with no swizzle after it,
    // which is exactly the shape of "passed or assigned whole" — so the first version of this test
    // certified every slot in the block as read, including the two it was written to find. Cutting the
    // declaration out is what leaves only USES behind.
    std::string WithoutTheBlockDeclaration( const std::string& code )
    {
        const std::size_t open = code.find( "readonly buffer CloudParamsBuffer" );
        if ( open == std::string::npos )
            return code;
        const std::size_t begin = code.find( '{', open );
        const std::size_t end   = code.find( '}', begin );
        if ( begin == std::string::npos || end == std::string::npos )
            return code;
        return code.substr( 0, open ) + code.substr( end + 1 );
    }

    // Component index for a swizzle letter, in all three GLSL vocabularies. -1 for anything else.
    int ComponentOf( char letter )
    {
        switch ( letter )
        {
            case 'x':
            case 'r':
            case 's':
                return 0;
            case 'y':
            case 'g':
            case 't':
                return 1;
            case 'z':
            case 'b':
            case 'p':
                return 2;
            case 'w':
            case 'a':
            case 'q':
                return 3;
            default:
                return -1;
        }
    }

    bool IsIdentifierChar( char c )
    {
        return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
    }
} // namespace

TEST( CloudMediumConsumers, EverySlotOfTheParameterBlockIsReadOrHasARowSayingWhyNot )
{
    const std::filesystem::path    shaders   = RepoRoot() / "Engine/Content/Shaders";
    const std::string              blockCode = CodeOnly( ReadAll( shaders / "Common/CloudParams.glslh" ) );
    const std::vector<BlockMember> members   = ParseBlock( blockCode );
    ASSERT_FALSE( members.empty() ) << "the parameter block could not be read, so this census counted nothing";

    // THE TWO HALVES OF ONE LAYOUT AGREE ON THEIR SIZE. Nothing else in the tree checks this: the C++
    // offsets are static_asserted against each other, and the GLSL is a mirror written by hand.
    int declaredFloats = 0;
    for ( const BlockMember& member : members )
        declaredFloats += member.Components * member.ArrayCount;
    EXPECT_EQ( declaredFloats, static_cast<int>( Desert::Graphic::kCloudPayloadFloats ) )
         << "Common/CloudParams.glslh declares " << declaredFloats << " floats and Graphic::CloudGpuPayload "
         << "carries " << Desert::Graphic::kCloudPayloadFloats
         << ". One of the two grew without the other, and every parameter after the difference is read from "
            "the wrong offset — which looks like badly tuned clouds and not like a bug.";

    // WHICH COMPONENTS ANY SHADER FETCHES, gathered over the whole tree: the block is included by four
    // programs and read inside a shared header as well, so no single file has the answer.
    std::map<std::string, std::set<int>> fetched;
    std::set<std::string>                sources;
    for ( const std::string& relative : ShaderFiles( shaders / "Programs", ".shader" ) )
        sources.insert( ( shaders / "Programs" / relative ).string() );
    for ( const std::string& relative : ShaderFiles( shaders / "Common", ".glslh" ) )
        sources.insert( ( shaders / "Common" / relative ).string() );

    for ( const std::string& path : sources )
    {
        const std::string code = WithoutTheBlockDeclaration( CodeOnly( ReadAll( path ) ) );
        for ( const BlockMember& member : members )
        {
            std::size_t at = 0;
            while ( ( at = code.find( member.Name, at ) ) != std::string::npos )
            {
                const std::size_t after = at + member.Name.size();
                // A longer identifier that merely STARTS with this name is a different member.
                const bool wholeWord = ( at == 0 || !IsIdentifierChar( code[at - 1] ) ) &&
                                       ( after >= code.size() || !IsIdentifierChar( code[after] ) );
                at = after;
                if ( !wholeWord )
                    continue;

                // A SUBSCRIPT OR A BARE USE READS THE WHOLE THING. `u_CloudSpeciesEdge[slot]` yields a
                // vec4 the caller then uses entire, and a bare member name is passed or assigned whole.
                if ( after >= code.size() || code[after] != '.' )
                {
                    for ( int c = 0; c < member.Components; ++c )
                        fetched[member.Name].insert( c );
                    continue;
                }

                // A swizzle: every letter of it is a fetch.
                for ( std::size_t s = after + 1; s < code.size(); ++s )
                {
                    const int component = ComponentOf( code[s] );
                    if ( component < 0 )
                        break;
                    if ( component < member.Components )
                        fetched[member.Name].insert( component );
                }
            }
        }
    }

    // THE REGISTER, as a set of (member, component) so both directions can be compared.
    std::set<std::pair<std::string, int>> registered;
    for ( const Desert::Graphic::CloudUnreadSlot& row : Desert::Graphic::kCloudUnreadSlots )
    {
        EXPECT_NE( row.Reason, nullptr ) << row.Member;
        EXPECT_STRNE( row.Reason, "" ) << row.Member << ": a row with an empty reason is not a row";
        registered.emplace( row.Member, ComponentOf( row.Component ) );
    }

    // ── EVERY UNFETCHED SLOT HAS A ROW ──────────────────────────────────────────────────────────────
    int unread = 0;
    for ( const BlockMember& member : members )
    {
        for ( int c = 0; c < member.Components; ++c )
        {
            const bool isFetched = fetched.count( member.Name ) != 0 && fetched[member.Name].count( c ) != 0;
            if ( isFetched )
                continue;

            ++unread;
            EXPECT_EQ( registered.count( { member.Name, c } ), 1u )
                 << member.Name << '.' << "xyzw"[c]
                 << " reaches the GPU and no shader reads it, and it has no row in "
                    "Graphic::kCloudUnreadSlots. Either read it, or remove it, or add a row saying why it "
                    "cannot be either — an unnamed slot is where the next parameter gets stashed with no "
                    "name, no range and no tooltip.";
        }
    }

    // ── AND EVERY ROW NAMES A SLOT THAT REALLY IS UNFETCHED ─────────────────────────────────────────
    for ( const Desert::Graphic::CloudUnreadSlot& row : Desert::Graphic::kCloudUnreadSlots )
    {
        const int  component = ComponentOf( row.Component );
        const bool isFetched = fetched.count( row.Member ) != 0 && fetched[row.Member].count( component ) != 0;
        EXPECT_FALSE( isFetched )
             << row.Member << '.' << row.Component
             << " has a row excusing it as unread, and a shader reads it. A stale exception is how a real "
                "dead slot gets to hide behind an old excuse — delete the row.";
    }

    // THE COUNT IS DERIVED FROM THE REGISTER, in both places, so the two cannot be adjusted independently:
    // this asserts the register's SIZE against the measurement, never a literal.
    EXPECT_EQ( unread, static_cast<int>( Desert::Graphic::kCloudUnreadSlots.size() ) );
    EXPECT_EQ( declaredFloats - unread, static_cast<int>( Desert::Graphic::kCloudPayloadReadFloats ) );

    std::printf( "[CloudMediumConsumers] parameter block: %d floats, %d read, %d registered as unread\n",
                 declaredFloats, declaredFloats - unread, unread );
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════
// WHICH MARCHES ARE SHADOW RAYS, AND THAT NOBODY CAN BUILD A MARCH WITHOUT SAYING
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════
//
// O1-F gave the medium the input the contract calls ShadowRay (O1_DESIGN §3.3): a march that only ever
// wants a transmittance may be answered cheaply, which is the same permission Epic grants through
// ShadowSampleDistance. The flag rides on CloudFieldParams rather than on the five entry points'
// signatures — one fact per MARCH instead of the same fact restated at eighteen call sites — and that
// choice buys a cheap change at the price of a question this census answers: what stops a march from
// simply not setting it?
//
// Two things, and both are asserted here.
//
//   1. THERE IS ONE CONSTRUCTOR. CloudUnpackFieldParams in Common/CloudParams.glslh is the only place in
//      the tree a CloudFieldParams is built, and it writes EVERY member. So "the flag is initialised" is
//      a property of one function rather than a rule four programs must remember, and a member added to
//      the struct and not packed there goes red below instead of arriving as an indeterminate float in a
//      medium the artist wrote.
//   2. THE DEFAULT IS THE EXPENSIVE ONE. That constructor writes CLOUD_RAY_VIEW, so a march that forgets
//      to declare itself runs the author's FULL medium: forgetting costs the saving that was asked for
//      and can never cost the frame its correctness. The opposite polarity would have made a new consumer
//      silently render a cheapened medium as though it were the real thing, which is not a defect anyone
//      would find by looking at a frame.
//
// The set of shadow contexts is then named, for the same reason the four consumers are: a FIFTH one is
// red on purpose, because whoever adds a march has to decide which kind it is in the same change.

TEST( CloudMediumConsumers, EveryFieldParameterIsWrittenByTheOnePlaceThatBuildsThem )
{
    const std::filesystem::path shaders = RepoRoot() / "Engine/Content/Shaders";
    const std::string           field   = CodeOnly( ReadAll( shaders / "Common/CloudField.glslh" ) );
    const std::string           params  = CodeOnly( ReadAll( shaders / "Common/CloudParams.glslh" ) );
    ASSERT_FALSE( field.empty() ) << "Common/CloudField.glslh was not found";
    ASSERT_FALSE( params.empty() ) << "Common/CloudParams.glslh was not found";

    // The members of `struct CloudFieldParams`, read out of the declaration itself. Each is the last
    // identifier of its statement, before any array brackets — the struct holds scalars, vectors and two
    // fixed-size arrays and nothing else.
    const std::size_t open = field.find( "struct CloudFieldParams" );
    ASSERT_NE( open, std::string::npos ) << "the field parameter struct is not declared where it was";
    const std::size_t close = field.find( "\n};", open );
    ASSERT_NE( close, std::string::npos ) << "the field parameter struct has no end";

    std::vector<std::string> members;
    std::istringstream       body( field.substr( open, close - open ) );
    for ( std::string line; std::getline( body, line ); )
    {
        const std::size_t semicolon = line.find( ';' );
        if ( semicolon == std::string::npos || line.find( "struct " ) != std::string::npos )
            continue;
        std::string statement = line.substr( 0, semicolon );
        if ( const std::size_t bracket = statement.find( '[' ); bracket != std::string::npos )
            statement = statement.substr( 0, bracket );
        std::size_t end = statement.find_last_not_of( " \t" );
        if ( end == std::string::npos )
            continue;
        const std::size_t begin = statement.find_last_of( " \t", end );
        if ( begin == std::string::npos )
            continue;
        members.push_back( statement.substr( begin + 1, end - begin ) );
    }
    ASSERT_GE( members.size(), 8u ) << "the struct was parsed as " << members.size()
                                    << " member(s), which means the scan above stopped meaning anything";

    // The packer's own body, so an assignment in a NEIGHBOURING function cannot stand in for one here.
    const std::size_t packer = params.find( "CloudFieldParams CloudUnpackFieldParams()" );
    ASSERT_NE( packer, std::string::npos ) << "the one place a CloudFieldParams is built has moved";
    const std::size_t packerEnd = params.find( "\n}", packer );
    ASSERT_NE( packerEnd, std::string::npos );
    const std::string packerBody = params.substr( packer, packerEnd - packer );

    std::vector<std::string> unwritten;
    for ( const std::string& member : members )
        if ( packerBody.find( "p." + member ) == std::string::npos )
            unwritten.push_back( member );

    EXPECT_TRUE( unwritten.empty() )
         << unwritten.size()
         << " member(s) of CloudFieldParams are never written by "
            "CloudUnpackFieldParams, so every march that calls it starts with an indeterminate value in "
            "them — and the medium an artist wrote is what reads it. The first is '"
         << ( unwritten.empty() ? std::string( "-" ) : unwritten.front() ) << "'.";

    std::printf( "[CloudMediumConsumers] %zu field parameters, all written by one packer\n", members.size() );
}

TEST( CloudMediumConsumers, TheShadowRayContextsAreTheThreeMarchesThatIntegrateOpticalDepth )
{
    const std::filesystem::path shaders = RepoRoot() / "Engine/Content/Shaders";

    // The safe polarity, at the one place it is decided.
    const std::string params = CodeOnly( ReadAll( shaders / "Common/CloudParams.glslh" ) );
    EXPECT_NE( params.find( "p.ShadowRay = CLOUD_RAY_VIEW" ), std::string::npos )
         << "the one constructor of CloudFieldParams no longer declares a view ray. A march that does not "
            "say what it is must get the FULL medium: with the polarity the other way round, a consumer "
            "added tomorrow would silently render whatever cheap approximation the artist wrote for "
            "shadows, and no frame would show it.";

    // Every file that declares itself a shadow ray, whether program or shared header.
    std::set<std::string> contexts;
    for ( const auto& [root, extension] : { std::pair<const char*, const char*>{ "Programs", ".shader" },
                                            std::pair<const char*, const char*>{ "Common", ".glslh" } } )
        for ( const std::string& relative : ShaderFiles( shaders / root, extension ) )
        {
            const std::string code = CodeOnly( ReadAll( shaders / root / relative ) );
            if ( code.find( "ShadowRay = CLOUD_RAY_SHADOW" ) != std::string::npos )
                contexts.insert( std::string( root ) + "/" + relative );
        }

    // THE THREE, NAMED, with what each of them integrates — the same form as the four consumers above and
    // for the same reason: a count would pass on the wrong three the day one is renamed.
    const std::set<std::string> expected = {
         // The sun quadrature. It is the only one that is not a whole program: it runs INSIDE the view
         // march and inside the sky bake, which is why the flag had to live on the params struct rather
         // than being a compile-time constant of a program.
         "Common/CloudField.glslh",
         "Programs/Clouds/CloudShadowMap.shader",          // the clouds' shadow on the ground
         "Programs/Clouds/CloudSkyOcclusionVolume.shader", // how much sky each column can see
    };

    EXPECT_EQ( contexts, expected )
         << "the set of marches that declare themselves shadow rays is not the three this tree knows "
            "about. A march ADDED here is one an authored medium may now answer cheaply, and it must be "
            "one that accumulates optical depth rather than radiance — the axis is what the loop SUMS, not "
            "where it points, which is why the sky-occlusion volume is on this list and the sky panorama "
            "bake is not. A march REMOVED here means an author's shadow saving quietly stopped applying "
            "to one of the three, and nothing in a frame says which.";

    std::printf( "[CloudMediumConsumers] %zu shadow-ray marches\n", contexts.size() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
