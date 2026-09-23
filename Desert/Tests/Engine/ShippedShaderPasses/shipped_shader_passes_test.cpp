// What every .shader the editor ships must be true of, checked against the engine's OWN DSL parser.
//
// WHY THIS SUITE EXISTS. `Unlit.shader` carried a `Pass "Depth"` for as long as the DSL had passes. It
// was commented as the shadow variant and it could not have been one: it declared NO fragment stage,
// while every attachment this engine renders into — the cascade included, which is a colour R32F target
// `Shadow.shader` WRITES with `gl_FragCoord.z` — has to be written by a fragment shader. Nothing named
// it either: `ShaderService` registers each named pass as its own program under "<Shader>/<Pass>", and
// no line of C++ has ever asked for one. So it was a whole SPIR-V module compiled at every startup for
// a program no draw could reach, and the file said the opposite of the truth to anyone reading it.
//
// Both halves of that are relations between two places, which is why they are here rather than in a
// comment: a pass and the target it would write, and a pass and the code that would address it. Each
// side alone reads as correct, and nothing was checking that they agreed.
//
// The parser is compiled into the test, so what is asserted is the same parse the runtime performs.

#include <gtest/gtest.h>

#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using Desert::Core::Formats::ShaderStage;
using Desert::Core::Preprocess::DShaderParser;
using Desert::Core::Preprocess::DShaderParseResult;

namespace
{
    struct ParsedShader
    {
        std::filesystem::path                 File;
        Common::ResultStr<DShaderParseResult> Parsed;
    };

    std::string ReadFile( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    // Every .shader under Editor/Resources/Shaders/Programs, parsed ONCE and sorted, so a failure names
    // the same file on every machine and the whole suite pays for one walk rather than one per test
    // (assembling a stage un-sugars every line, which is not free over seventy-odd files). The root is
    // found by walking up from the test binary in build/Bin/Tests/<config>, as Tests/Engine/ShaderCacheKey
    // does.
    const std::vector<ParsedShader>& ShippedShaders()
    {
        static const std::vector<ParsedShader> shaders = []() -> std::vector<ParsedShader>
        {
            std::filesystem::path here = std::filesystem::current_path();
            for ( int up = 0; up < 8 && !std::filesystem::exists( here / "Editor" / "Resources" / "Shaders" );
                  ++up )
                here = here.parent_path();

            std::vector<std::filesystem::path> files;
            const auto                         root = here / "Editor" / "Resources" / "Shaders" / "Programs";
            if ( std::filesystem::exists( root ) )
                for ( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
                    if ( entry.is_regular_file() && entry.path().extension() == ".shader" )
                        files.push_back( entry.path() );
            std::sort( files.begin(), files.end() );

            std::vector<ParsedShader> out;
            out.reserve( files.size() );
            for ( const auto& file : files )
                out.push_back( { file, DShaderParser::Parse( ReadFile( file ) ) } );
            return out;
        }();
        return shaders;
    }
} // namespace

// The shipped shaders are data, and nothing else in the repository parses all of them: ShaderCacheKey
// reaches six by name and the editor reads the rest for the first time at startup. A syntax error in one
// is a shader that does not draw, reported only in a log nobody is reading.
TEST( ShippedShaderPasses, EveryShippedShaderParses )
{
    ASSERT_FALSE( ShippedShaders().empty() ) << "found no .shader files — the shader root was not located";

    for ( const auto& shader : ShippedShaders() )
        EXPECT_TRUE( shader.Parsed.IsSuccess() ) << shader.File.string() << ": " << shader.Parsed.GetError();
}

// THE RELATION: a pass that rasterizes writes an attachment, and only a fragment stage can write one.
// A vertex stage with no fragment stage beside it produces a program that runs and emits nothing —
// which is exactly what `Unlit/Depth` was, and why calling it a shadow caster was never true. Compute
// passes are exempt because they write through storage bindings instead.
TEST( ShippedShaderPasses, EveryRasterPassCanWriteWhatItRendersInto )
{
    for ( const auto& shader : ShippedShaders() )
    {
        ASSERT_TRUE( shader.Parsed.IsSuccess() ) << shader.File.string() << ": " << shader.Parsed.GetError();

        for ( const auto& pass : shader.Parsed.GetValue().Passes )
        {
            if ( !pass.Stages.count( ShaderStage::Vertex ) )
                continue;

            const std::string label = pass.Name.empty() ? std::string( "<default pass>" ) : pass.Name;
            EXPECT_TRUE( pass.Stages.count( ShaderStage::Fragment ) )
                 << shader.File.string() << ", pass " << label
                 << ": rasterizes but declares no fragment stage, so it writes nothing to the target it "
                    "is rendered into";
        }
    }
}

// THE OTHER RELATION: a named pass is a separate program, addressable only as "<Shader>/<Pass>" through
// ShaderService::GetByName — and no consumer in the engine asks for one. So a shipped shader that
// declares a named pass ships a program nothing can reach: it is compiled at every startup and drawn by
// nobody, and the reader of the file concludes it means something.
//
// WHEN THIS FAILS, THE FIX IS NOT TO RELAX IT. Either the pass has a consumer — then this test names the
// call site that must exist, and the pass belongs in an expected set added here beside it — or it has
// none, and the pass is the thing to delete.
TEST( ShippedShaderPasses, NoShippedShaderDeclaresAPassNothingCanAddress )
{
    for ( const auto& shader : ShippedShaders() )
    {
        ASSERT_TRUE( shader.Parsed.IsSuccess() ) << shader.File.string() << ": " << shader.Parsed.GetError();

        for ( const auto& passName : shader.Parsed.GetValue().Meta.PassNames )
            ADD_FAILURE() << shader.File.string() << ": declares Pass \"" << passName
                          << "\", which registers a program named \""
                          << shader.Parsed.GetValue().Name + "/" + passName
                          << "\" that no GetByName call in the engine ever asks for";
    }
}

// ---- ONE PARAMETER TRANSPORT, asserted as a relation ---------------------------------------------------
//
// There used to be two ways a material's parameters reached a draw, and this was a CENSUS of which shipped
// shaders used which — a count that was expected to move to zero when the transports were collapsed. They
// are collapsed, so it is a rule now: whatever built a material, its parameters travel one way.
//
//   THE ONE THAT SURVIVED. A row of a shared `Materials[]` storage buffer, named by a `MaterialIndex` push
//   constant at Core::Formats::kMaterialIndexPushOffset. N objects with N different parameter sets record
//   N draws against ONE descriptor set, because each draw carries its own index and Vulkan snapshots a
//   push at record time.
//
//   THE ONE THAT WAS DELETED. A per-material uniform block, generated by the DSL's `Properties Binding(n)`
//   as `uniform MaterialUB ... u_Material`. The block IS the parameters, so a material held exactly ONE
//   set of values — and MeshRenderer keys one material per shader, so several objects drawn with one
//   shader all rendered the values of whichever draw wrote last.
//
// THE DEFECT IT SHIPPED, because a rule without its evidence gets relaxed by the next person to trip on
// it. Resources/Assets/Scenes/MAT_ProbeSharedBlock.desce is three spheres whose MatProbe `Blend` is 0.0,
// 0.5 and 1.0; on the old transport all three rendered RED, the Blend = 0 colour. The control,
// MAT_ProbeSharedBlockSingle.desce — the same entity alone at Blend = 1.0 — rendered BLUE, so the override
// path worked and what broke the three was the sharing. The surviving transport has no such failure mode
// by construction; it is the same reason the bone matrices were moved off the skinned material.
//
// AND WHY THE COLLAPSE WENT THIS WAY ROUND rather than the simpler one, measured 2026-09-04 in Debug,
// reading the mesh pass's own GPU-timestamp line and taking the minimum of interleaved runs:
//
//   scene                          draws   MeshGeometryPass CPU (min of N)     frame (wall, min of N)
//   MAT_ProbeCascadeSeam (101)     2 vs 101   0.259 ms  ->  1.855 ms  (7.2x)   8.379 -> 8.336 ms (in slack)
//   MAT_ProbeBatchStress (1025)    2 vs 1025  0.756 ms  -> 17.121 ms (22.6x)  11.477 -> 27.973 ms (2.4x)
//
// Putting everything on the per-material block would have cost 2.4x the frame at a thousand meshes AND
// taken the wrong answer above with it.
TEST( ShippedShaderPasses, TheParameterTransportCensusIsWhatItWasLeftAt )
{
    std::vector<std::string> perMaterialBlock;

    for ( const auto& shader : ShippedShaders() )
    {
        ASSERT_TRUE( shader.Parsed.IsSuccess() ) << shader.File.string() << ": " << shader.Parsed.GetError();

        // The marker is the DECLARATION the parser generates, not the source text: a shader that writes
        // the same block by hand is the same transport and must be counted the same way. Terrain.shader
        // did exactly that and is why this is spelled as a search rather than as a parser flag.
        for ( const auto& [stage, source] : shader.Parsed.GetValue().Stages )
        {
            if ( source.find( "uniform MaterialUB" ) != std::string::npos )
            {
                perMaterialBlock.push_back( shader.File.filename().string() );
                break;
            }
        }
    }

    std::sort( perMaterialBlock.begin(), perMaterialBlock.end() );

    EXPECT_EQ( perMaterialBlock, std::vector<std::string>{} )
         << "a shipped shader is back on the per-material uniform block. It cannot give two objects "
            "different parameter values — see MAT_ProbeSharedBlock.desce above — and the transport it "
            "belongs on is a row of Materials[], which `Properties Binding(n)` generates for free.";
}

// THE RELATION the census above is only half of: a shader that READS material parameters must DECLARE the
// buffer they come from. Counting the losing transport at zero would still pass if somebody invented a
// third one, and "a third one" is not hypothetical — the terrain's block was hand-written, not generated,
// and no parser flag would have found it.
TEST( ShippedShaderPasses, EveryShaderThatReadsMaterialParametersReadsThemFromTheSharedRowBuffer )
{
    const std::string rowBuffer = std::string( "buffer " ) + Desert::Core::Formats::kMaterialRowBlockName;

    std::vector<std::string> onTheSharedBuffer;
    std::vector<std::string> readsWithoutDeclaring;

    for ( const auto& shader : ShippedShaders() )
    {
        ASSERT_TRUE( shader.Parsed.IsSuccess() ) << shader.File.string() << ": " << shader.Parsed.GetError();

        bool declares = false;
        bool reads    = false;
        for ( const auto& [stage, source] : shader.Parsed.GetValue().Stages )
        {
            declares = declares || source.find( rowBuffer ) != std::string::npos;
            reads    = reads || source.find( "u_Material" ) != std::string::npos;
        }

        if ( declares )
            onTheSharedBuffer.push_back( shader.File.filename().string() );
        else if ( reads )
            readsWithoutDeclaring.push_back( shader.File.filename().string() );
    }

    std::sort( readsWithoutDeclaring.begin(), readsWithoutDeclaring.end() );
    EXPECT_EQ( readsWithoutDeclaring, std::vector<std::string>{} )
         << "a shipped shader reads u_Material without declaring the Materials[] buffer it comes from, so "
            "it is carrying its parameters some third way.";

    // The six that migrated, named so this test fails if one of them silently stops carrying parameters
    // at all — which is how a transport change quietly turns into a shader that renders its defaults.
    for ( const char* migrated : { "MatProbe.shader", "MatProbeUnlit.shader", "NewShaderGraph.shader",
                                   "Terrain.shader", "TextSDF.shader", "Unlit.shader" } )
    {
        EXPECT_NE( std::find( onTheSharedBuffer.begin(), onTheSharedBuffer.end(), migrated ),
                   onTheSharedBuffer.end() )
             << migrated << " no longer declares the shared Materials[] buffer";
    }
}

// The push constant that names the row is what makes the transport per-DRAW rather than per-material, so a
// shader that declares the buffer and forgets the index would compile, render, and be wrong in exactly the
// way the old transport was. It comes from ONE include, and this asserts every generated block took it.
TEST( ShippedShaderPasses, AGeneratedMaterialRowAlwaysArrivesWithThePushConstantThatIndexesIt )
{
    const std::string rowBuffer = std::string( "buffer " ) + Desert::Core::Formats::kMaterialRowBlockName;

    std::set<std::string> withAGeneratedBlock;

    for ( const auto& shader : ShippedShaders() )
    {
        ASSERT_TRUE( shader.Parsed.IsSuccess() ) << shader.File.string() << ": " << shader.Parsed.GetError();

        for ( const auto& [stage, source] : shader.Parsed.GetValue().Stages )
        {
            if ( source.find( "#define u_Material u_Materials[" ) == std::string::npos )
                continue; // not a GENERATED block; a hand-written one indexes itself

            withAGeneratedBlock.insert( shader.File.filename().string() );

            EXPECT_NE( source.find( rowBuffer ), std::string::npos )
                 << shader.File.string() << ": defines u_Material without the buffer behind it";
            EXPECT_NE( source.find( "#include <Common/MaterialTransport.glslh>" ), std::string::npos )
                 << shader.File.string()
                 << ": indexes Materials[] with m_PushConstants.MaterialIndex but never declares the push "
                    "block that carries it";
        }
    }

    // Everything above is inside `if (marker found) ... else continue`, so it asserts nothing at all when
    // the marker stops matching — and the marker is a literal spelling of generated text. Change how the
    // emitter writes that #define and this test goes green by having no work to do, which is the one
    // failure mode a census must not have. The sibling test (…ReadsThemFromTheSharedRowBuffer) does not
    // cover it: that one keys on "buffer Materials", a different string, so a #define respelling leaves it
    // green too.
    // WHY A NAMED REGISTER AND NOT A COUNT. This assertion used to pin the NUMBER six, and a gate pinned
    // by a number is satisfied by editing the number: Ю11 added a seventh shipped shader and the honest
    // repair looked identical to the dishonest one. Naming each row makes an addition state WHICH shader
    // was added, and the count is derived from the list rather than maintained beside it.
    // Ю11 added the last of these. UIMatError.shader is deliberately ABSENT: it declares no resources at
    // all, which is what makes it the tree's only test of a resource-free program, so it carries no row.
    static constexpr std::string_view kCarriesAGeneratedRow[] = {
         "MatProbe.shader", "MatProbeUnlit.shader",   "NewShaderGraph.shader", "Terrain.shader",
         "TextSDF.shader",  "UIMatRadialWipe.shader", "Unlit.shader",
    };

    std::set<std::string> expected;
    for ( const std::string_view name : kCarriesAGeneratedRow )
        expected.emplace( name );

    EXPECT_EQ( withAGeneratedBlock, expected )
         << "the set of shipped shaders carrying a generated material row is not the registered one. Add or"
            " remove the NAME above, never a count. If the set is empty or unexpectedly small, the marker"
            " string above has stopped matching the emitter and this test was passing without examining"
            " anything.";
}

// THE THIRD RELATION: what a shader SAYS and what a shader DECLARES are two different texts, and the
// sugar may only rewrite the second. In / Out / Uniform / Buffer / PushConstant are ordinary English
// words, and the paren-less rules used to match them anywhere in the file — so six shipped shaders had
// a sentence of their own documentation compiled into a layout qualifier, each one silently consuming
// an automatic number:
//
//   Particles/ParticleSimulate    "// Integrate alive particles. In LOCAL mode (u_Counts.w) ..."
//   Sky/SkyDistantLight           "// ... two LUT fetches per step. In one thread that is ~2000 ..."
//   Deferred/DeferredLighting     "// ... many dynamic lights in screen space. In a debug mode it ..."
//   Fog/HeightFog                 "// ... is what fades this in. In the physical ..."
//   Clouds/CloudShadowMap         "// WHERE IT RUNS. In-frame compute, outside any render pass ..."
//   Clouds/CloudSkyOcclusionVolume  same sentence
//
// AND THE INCLUDES TOO, which is why this test walks `.glslh` as well as `.shader`. ShaderIncluder.cpp
// runs TranslateSugar over every included file, so a shared header is sugared exactly like a stage —
// Common/CloudAuthored.glslh ("In RGBA8 that is 4.00 MiB", "In FLOAT, a") and Common/SkyMedium.glslh
// ("// ---- Uniform sphere sampling ----", which took a BINDING, not a location) were doing it too. A
// section heading is the likeliest place of all for one of these words, and no shader author would ever
// look there.
//
// None of the nine was visible in a frame, because none of those texts declares an automatic In / Out /
// Uniform / Buffer — the eaten number was never asked for. That is precisely why it needed a test rather
// than a habit: the cost lands on whoever adds the first automatic declaration to one of them, and it
// lands as attributes shifted by one, in the code, with the cause in the prose above it.
//
// This is checked over the SHIPPED TREE and not only in the parser's unit tests because the alternative
// is a convention — "do not write these words in comments" — that nobody can be told and no reviewer can
// see. The comment scanner below is deliberately a SECOND implementation of "where the comments are":
// if it and the parser's ever disagree, this test is what says so.
namespace
{
    // The comment runs of a GLSL text: `//` to end of line, `/* */` non-nesting. Nothing else — the
    // language has no string literals, and `#if 0` is code (see DShaderParserComments).
    std::vector<std::string> CommentRuns( const std::string& text )
    {
        std::vector<std::string> runs;
        for ( size_t i = 0; i + 1 < text.size(); )
        {
            if ( text[i] == '/' && text[i + 1] == '/' )
            {
                const size_t end  = text.find( '\n', i );
                const size_t stop = ( end == std::string::npos ) ? text.size() : end;
                runs.push_back( text.substr( i, stop - i ) );
                i = stop;
            }
            else if ( text[i] == '/' && text[i + 1] == '*' )
            {
                const size_t close = text.find( "*/", i + 2 );
                const size_t stop  = ( close == std::string::npos ) ? text.size() : close + 2;
                runs.push_back( text.substr( i, stop - i ) );
                i = stop;
            }
            else
            {
                ++i;
            }
        }
        return runs;
    }

    // A capitalized sugar keyword standing alone in prose — the shape that used to be rewritten.
    bool MentionsASugarKeyword( const std::string& comment )
    {
        static const std::regex kKeyword( R"(\b(In|Out|Uniform|Buffer|ReadBuffer|WriteBuffer|PushConstant)\b)" );
        return std::regex_search( comment, kKeyword );
    }

    // Every assembled GLSL text a shader produces: the default stages plus every pass's stages.
    std::vector<std::string> AllStageTexts( const DShaderParseResult& parsed )
    {
        std::vector<std::string> texts;
        for ( const auto& [stage, source] : parsed.Stages )
            texts.push_back( source );
        for ( const auto& pass : parsed.Passes )
            for ( const auto& [stage, source] : pass.Stages )
                texts.push_back( source );
        return texts;
    }

    // Every `.glslh` under the shader root — Common/ and Mesh/, not only Programs/. These never reach
    // DShaderParser::Parse; the compiler's includer hands each one to TranslateSugar on its own
    // (ShaderIncluder.cpp), which is what this reproduces.
    const std::vector<std::filesystem::path>& ShippedIncludes()
    {
        static const std::vector<std::filesystem::path> files = []()
        {
            std::filesystem::path here = std::filesystem::current_path();
            for ( int up = 0; up < 8 && !std::filesystem::exists( here / "Editor" / "Resources" / "Shaders" );
                  ++up )
                here = here.parent_path();

            std::vector<std::filesystem::path> out;
            const auto                         root = here / "Editor" / "Resources" / "Shaders";
            if ( std::filesystem::exists( root ) )
                for ( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
                    if ( entry.is_regular_file() && entry.path().extension() == ".glslh" )
                        out.push_back( entry.path() );
            std::sort( out.begin(), out.end() );
            return out;
        }();
        return files;
    }
} // namespace

// ── A NORMAL MAP IS READ IN EXACTLY ONE WAY ───────────────────────────────────────────────────────
//
// An artist can now mark a texture `NormalMap` (`Core/Formats/TextureIntent.hpp`), and the cook then
// stores it as BC5 — two channels. A sampler returns BC5 as (X, Y, 0, 1), so a shader that reads `.rgb`
// and re-centres it gets a Z of MINUS ONE everywhere: every lit surface wrong, no error anywhere, and
// only on the textures somebody took the trouble to classify.
//
// `Common/TangentNormal.glslh` reconstructs Z and is the only correct reading. This census is what stops
// a sixth shader being written with the old line, or one of the five being edited back — which is a
// realistic mistake, because the old line is what every reference on the internet says and it is right
// for every format except the one the cook now produces.
//
// IT IS A CENSUS AND NOT A FRAME, and the reason is worth writing down: no `.desce` in this repository
// binds a normal map at all (one material names `u_NormalTexture` and no scene references it), so there
// is no shot that would go red if this were wrong. The first scene that draws a normal-mapped surface is
// what will confirm the arithmetic; until then this holds the SHAPE.
TEST( ShippedShaderPasses, EveryShaderThatReadsANormalMapGoesThroughTheSharedReconstruction )
{
    ASSERT_FALSE( ShippedShaders().empty() );

    int readers = 0;
    for ( const ParsedShader& shader : ShippedShaders() )
    {
        const std::string text = ReadFile( shader.File );
        if ( text.find( "u_NormalTexture" ) == std::string::npos )
            continue;
        ++readers;

        EXPECT_NE( text.find( "#include <Common/TangentNormal.glslh>" ), std::string::npos )
             << shader.File.filename().string()
             << " samples a normal map without including the shared reconstruction";
        EXPECT_NE( text.find( "SampleTangentNormal(u_NormalTexture" ), std::string::npos )
             << shader.File.filename().string() << " does not read its normal map through "
             << "SampleTangentNormal; a BC5 normal map decodes to a Z of -1 through any other reading";

        // THE OLD SPELLING, BY NAME. `.rgb` off a normal-map sampler is the defect, whatever the
        // arithmetic around it looks like, because the third channel is not there to read.
        const std::size_t sampled = text.find( "texture(u_NormalTexture" );
        EXPECT_EQ( sampled, std::string::npos )
             << shader.File.filename().string()
             << " samples u_NormalTexture directly instead of through SampleTangentNormal";
    }

    // DERIVED, AND IT HAS TO BE NON-ZERO. A census over a set that turned out to be empty passes
    // silently, and this one would the day somebody renamed the uniform.
    EXPECT_GT( readers, 0 ) << "no shipped shader reads a normal map any more — either the uniform was "
                               "renamed, or this census is looking at the wrong tree";
    std::cout << "[  CENSUS  ] " << readers << " shipped shader(s) read a normal map, all through "
              << "Common/TangentNormal.glslh\n";
}

TEST( ShippedShaderPasses, NoShippedShaderTranslatesItsOwnProse )
{
    int filesWithProseKeywords = 0;

    // THE PREDICATE IS "VERBATIM", NOT "CONTAINS layout(". The first spelling of this test looked for
    // `layout(` inside an output comment and reported two false positives immediately: CloudParams.glslh
    // and FogParams.glslh both DOCUMENT the raw form — "Raw `layout(std430, ...)` rather than the
    // ReadBuffer(n) sugar" — so the string a human typed was indistinguishable from one the sugar wrote.
    // A comment the sugar left alone is present, character for character, in the file it came from; a
    // rewritten one is not. That has no false positives and needs no list of exceptions.
    const auto check =
         [&filesWithProseKeywords]( const std::filesystem::path& file, const std::vector<std::string>& translated )
    {
        const std::string source = ReadFile( file );

        bool mentions = false;
        for ( const auto& comment : CommentRuns( source ) )
            mentions = mentions || MentionsASugarKeyword( comment );
        filesWithProseKeywords += mentions ? 1 : 0;

        for ( const auto& text : translated )
            for ( const auto& comment : CommentRuns( text ) )
                EXPECT_NE( source.find( comment ), std::string::npos )
                     << file.string() << ": this comment came out of the translation in a form the file"
                     << " does not contain, so the sugar rewrote a word of PROSE — which also consumed an"
                     << " automatic location/binding. Offending comment:\n"
                     << comment;
    };

    for ( const auto& shader : ShippedShaders() )
    {
        ASSERT_TRUE( shader.Parsed.IsSuccess() ) << shader.File.string() << ": " << shader.Parsed.GetError();
        check( shader.File, AllStageTexts( shader.Parsed.GetValue() ) );
    }

    ASSERT_FALSE( ShippedIncludes().empty() ) << "found no .glslh files — the shader root was not located";
    for ( const auto& include : ShippedIncludes() )
        check( include, { DShaderParser::TranslateSugar( ReadFile( include ) ) } );

    // The check must have something to examine. If a future cleanup deletes every mention of the six
    // words from every shipped comment, this test would go green by having nothing to look at — the one
    // failure mode a census must not have. It is a floor, not a pinned count: the whole point of the fix
    // is that an author may now write "In LOCAL mode" without thinking about it, so the number is free
    // to grow.
    EXPECT_GE( filesWithProseKeywords, 14 )
         << "shipped shaders and includes stopped mentioning sugar keywords in prose, so this test"
            " examined nothing. Either add such a comment back, or delete this test and say why in the"
            " commit.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
