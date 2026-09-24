// DOES EVERY FIELD THE SHADER PARSER FILLS REACH ANYTHING?
//
// WHY IT EXISTS. `ShaderParam::DefaultTexture` was found by accident: the DSL parser read `= "white"`
// off a Texture2D property, stored it, and no line in the engine or the editor ever asked for it. The
// consequence was not a cosmetic one — it was the reason a texture could not be UNBOUND at all, because
// "empty slot" has no meaning without the value that says what an empty slot looks like (М9). The field
// had been sitting there since the DSL was written.
//
// That is the same shape SettingConsumers (Д23) and ConfigOwnership (К1) were built for, one layer down:
// a producer fills a structure, a consumer is assumed, and nothing anywhere states the relation. So this
// suite states it for the shader schema — `ShaderParam`, `ShaderRenderState`, `ShaderProgramMeta` and the
// parser's own `DShaderParseResult`/`DShaderPass` — the same way, and DELIBERATELY NOT IN A SIXTH STYLE:
// the text machinery is `setting_consumers_reader.hpp`, included from the suite next door.
//
// WHAT A ROW ASSERTS, AND WHAT IT DOES NOT. `SettingConsumers` can anchor a read to a receiver it proves
// is of the right type, because an ECS component is reached through a named wrapper. A schema param is
// not: every consumer in this engine reaches one as `for ( const auto& p : schema.Params )`, so the type
// name never appears near the read and an anchored receiver is not derivable. A WIRED row therefore
// asserts something weaker and honest about it:
//
//   * the named file really SEES the schema (it names `ShaderProgramMeta`, `GetProgramMeta`, `GetSchema`
//     or `DShaderParser`), and
//   * that file contains a member READ of the field — `x.Field` / `x->Field` not immediately followed by
//     a plain `=` — outside comments and string literals.
//
// So it is file-granular, not expression-granular. It catches the thing it was written for (a field with
// NO consumer anywhere) and it would not catch a consumer that stopped reading one field while still
// reading its neighbours in the same file. Saying which is which is the point; a census that overclaims
// is worse than none.
//
// The PRODUCER side is asserted too, and it is what makes each row a relation rather than an opinion:
// every field must also be touched by `DShaderParser.cpp`. A field nothing writes AND nothing reads is a
// different and larger defect than a dead one, and this suite must not report it as the same thing.

#include "setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace CT = Desert::Tests::ConsumerText;

    // Walks up from the working directory looking for a file only the repository has, as
    // PureVirtualCensus and DeviceLostCensus do for the same reason: the runner's working directory is
    // not fixed. (The suites share no header for this; copy-paste is the convention this directory
    // follows.)
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/Formats/ShaderProgramMeta.hpp" );
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
        std::string text = buffer.str();
        if ( text.compare( 0, 3, "\xEF\xBB\xBF" ) == 0 )
            text.erase( 0, 3 );
        return text;
    }

    // A CHARACTER LITERAL HOLDING A QUOTE ONCE ATE THE REST OF THE FILE, and this cost М9 an afternoon.
    // `ConsumerText::StripCommentsAndLiterals` knew about `"…"` and about comments and NOT about `'…'`,
    // so the perfectly ordinary `c.Peek() == '"'` in DShaderParser.cpp opened a string literal that ran
    // to the next quote hundreds of lines away — and every field written in between was reported as never
    // touched by the parser. Six rows of this census went red for a defect in the census.
    //
    // М9 worked around it HERE, with a pre-pass that blanked character literals before handing the text
    // to the shared stripper, and said in this comment that the suite next door had the same hole. Д33
    // fixed the shared stripper instead — character literals, raw strings and encoding prefixes are all
    // part of it now, and each form is pinned by a table in `SettingConsumers`. The pre-pass is gone
    // rather than kept as a belt-and-braces second layer: two places that both decide what a literal is
    // are two places that can disagree, and the workaround was itself blind to raw strings.
    std::string Strip( const std::string& src )
    {
        return CT::StripCommentsAndLiterals( src );
    }

    // ---- Enumerating a struct's data members out of its header -----------------------------------
    //
    // MECHANICAL ON PURPOSE. A hand-typed field list is the thing this suite exists to stop being
    // trusted: the day somebody adds a field to ShaderParam, the list that was typed by hand is exactly
    // the one that does not grow, and the census certifies a schema it no longer describes.

    // The body of `struct <name> { ... }` at brace depth balance, or empty when the struct is absent.
    std::string StructBody( const std::string& source, const std::string& name )
    {
        const std::string needle = "struct " + name;
        for ( std::size_t at = source.find( needle ); at != std::string::npos; at = source.find( needle, at + 1 ) )
        {
            if ( !CT::WordAt( source, at + 7, name ) )
                continue;

            const std::size_t open = source.find( '{', at );
            if ( open == std::string::npos )
                continue;

            int depth = 0;
            for ( std::size_t i = open; i < source.size(); ++i )
            {
                if ( source[i] == '{' )
                    ++depth;
                else if ( source[i] == '}' && --depth == 0 )
                    return source.substr( open + 1, i - open - 1 );
            }
        }
        return {};
    }

    // Data members of a struct body (comments already stripped by the caller): at brace depth 0 and
    // OUTSIDE template argument lists, the last identifier of a declaration before `;` or `=`. A
    // statement containing `(` before its terminator is a member FUNCTION and is skipped, which is what
    // keeps `bool IsAssetRef() const` out.
    //
    // The angle-bracket depth is not decoration. Without it `std::optional<StateCull> Cull;` yields
    // `StateCull` — a TYPE reported as a field — and the census then demands a consumer for a name that
    // is not in the structure at all. It went red that way first.
    std::vector<std::string> DataMembers( const std::string& body )
    {
        std::vector<std::string> fields;

        int         depth    = 0;
        int         angle    = 0;
        bool        sawParen = false;
        std::string lastIdent;
        std::string pending;

        const auto flush = [&]()
        {
            if ( !sawParen && !lastIdent.empty() )
            {
                // Keywords a declaration-looking run can end on. `const` and friends cannot be a member
                // name, and a run that ends on one is not a declaration this census is about.
                static const std::set<std::string> ignore = { "public", "private", "protected", "struct", "class",
                                                              "enum",   "using",   "return",    "const",  "static",
                                                              "inline", "bool",    "int",       "float",  "double",
                                                              "char",   "void",    "unsigned" };
                if ( ignore.count( lastIdent ) == 0 )
                    fields.push_back( lastIdent );
            }
            sawParen = false;
            lastIdent.clear();
            pending.clear();
        };

        const auto closePending = [&]( std::size_t i )
        {
            if ( pending.empty() )
                return;
            // `A::B` is a qualified type name, never a member name.
            std::size_t j = i;
            while ( j < body.size() && std::isspace( static_cast<unsigned char>( body[j] ) ) != 0 )
                ++j;
            const bool qualified = j + 1 < body.size() && body[j] == ':' && body[j + 1] == ':';
            if ( !qualified && angle == 0 )
                lastIdent = pending;
            pending.clear();
        };

        for ( std::size_t i = 0; i < body.size(); ++i )
        {
            const char c = body[i];

            if ( c == '{' )
            {
                closePending( i );
                ++depth;
                continue;
            }
            if ( c == '}' )
            {
                closePending( i );
                --depth;
                continue;
            }
            if ( depth != 0 )
                continue;

            if ( c == '<' )
            {
                closePending( i );
                ++angle;
                continue;
            }
            if ( c == '>' )
            {
                closePending( i );
                if ( angle > 0 )
                    --angle;
                continue;
            }
            if ( c == '(' || c == ')' )
            {
                closePending( i );
                sawParen = true;
                continue;
            }
            if ( c == ';' )
            {
                closePending( i );
                flush();
                continue;
            }
            if ( c == '=' )
            {
                // Everything after `=` is an initialiser; the member name is already behind us.
                closePending( i );
                // ...unless the `=` is part of an operator's NAME (`operator==`, `operator<=>`): the `(`
                // that marks a member function comes after it, so without this the declaration
                // `bool operator==( ... ) const = default;` read as a data member called `operator`.
                if ( lastIdent == "operator" )
                {
                    sawParen = true;
                    angle    = 0;
                }
                while ( i < body.size() && body[i] != ';' )
                    ++i;
                flush();
                continue;
            }
            if ( CT::IsIdentChar( c ) )
            {
                pending += c;
                continue;
            }
            closePending( i );
        }
        flush();

        return fields;
    }

    // A member READ of `field` anywhere in `text`: `x.Field` or `x->Field`, not immediately followed by
    // a plain `=`. MemberReadAt is the same predicate SettingConsumers uses, so "a write is not a read"
    // means exactly what it means there.
    bool ContainsMemberRead( const std::string& text, const std::string& field )
    {
        for ( std::size_t at = text.find( field ); at != std::string::npos; at = text.find( field, at + 1 ) )
        {
            if ( at == 0 )
                continue;
            std::size_t before = at;
            if ( text[before - 1] == '.' )
                --before;
            else if ( before >= 2 && text[before - 1] == '>' && text[before - 2] == '-' )
                before -= 2;
            else
                continue;

            if ( CT::MemberReadAt( text, before, field ) )
                return true;
        }
        return false;
    }

    bool ContainsMemberTouch( const std::string& text, const std::string& field )
    {
        for ( std::size_t at = text.find( field ); at != std::string::npos; at = text.find( field, at + 1 ) )
        {
            if ( at == 0 || !CT::WordAt( text, at, field ) )
                continue;
            if ( text[at - 1] == '.' )
                return true;
            if ( at >= 2 && text[at - 1] == '>' && text[at - 2] == '-' )
                return true;
        }
        return false;
    }

    // ---- The register ------------------------------------------------------------------------------

    struct Row
    {
        const char* Struct;
        const char* Field;
        const char* Where; ///< WIRED: repo-relative file that must contain a member read.
        const char* Dead;  ///< DEAD: why nothing reads it, and who decides. Exactly one of the two.
    };

    constexpr const char* kParamRow  = "Desert/Desert/Source/Engine/Core/Formats/MaterialParamRow.hpp";
    constexpr const char* kFactory   = "Desert/Desert/Source/Engine/Graphic/Materials/MaterialFactory.cpp";
    constexpr const char* kMaterial  = "Desert/Desert/Source/Engine/Graphic/Materials/Material.cpp";
    constexpr const char* kDDM       = "Desert/Desert/Source/Engine/Graphic/Materials/DataDrivenMaterial.hpp";
    constexpr const char* kPipeline  = "Desert/Desert/Source/Engine/Graphic/PipelineCache.hpp";
    constexpr const char* kMeshRend  = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp";
    constexpr const char* kShaderSvc = "Desert/Desert/Source/Engine/Runtime/Services/Shader/ShaderService.cpp";
    constexpr const char* kPreproc =
         "Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.cpp";
    constexpr const char* kParser  = "Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp";
    constexpr const char* kMatEdit = "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp";
    constexpr const char* kMatEditStates = "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditStates.hpp";

    constexpr Row k_Census[] = {
         // ---- ShaderParam: the per-property schema --------------------------------------------------
         { "ShaderParam", "Name", kParamRow, nullptr },
         { "ShaderParam", "DisplayName", kMatEdit, nullptr },

         // THE ROW THAT WAS DEAD AND IS NOT ANY MORE. It read "OWNER DECIDES / FENCED: 52 authored
         // categories, and the panel draws one flat table" — true when it was written, and stale since О9:
         // MaterialEdit::PlanParameterGroups reads this field and the Material Editor draws the parameter
         // table in the author's own groups, in the shader file's declaration order. Rewired rather than
         // deleted, because the two states of the row are the whole record of the fix.
         { "ShaderParam", "Category", kMatEditStates, nullptr },

         // WHEN AN EDIT TO THIS PARAMETER REACHES THE PICTURE (O1). Read by
         // MaterialEdit::FoldGroupTiming, which gives every parameter group's heading its cost, and by
         // DescribeProperties, which puts it on the control channel — a `Rebake` write is seconds away
         // from being visible, and a client that does not know which kind it wrote reads the unchanged
         // frame as a failure.
         { "ShaderParam", "Timing", kMatEditStates, nullptr },

         { "ShaderParam", "Tooltip", kMatEdit, nullptr },
         { "ShaderParam", "Type", kMatEditStates, nullptr },
         { "ShaderParam", "Widget", kMatEdit, nullptr },
         { "ShaderParam", "IsTexture", kParamRow, nullptr },
         { "ShaderParam", "AssetKind", kMatEdit, nullptr },
         { "ShaderParam", "IsCubeTexture", kFactory, nullptr },
         { "ShaderParam", "Min", kMatEdit, nullptr },
         { "ShaderParam", "Max", kMatEdit, nullptr },
         { "ShaderParam", "Default", kDDM, nullptr },

         // The row this suite was born from. Read since М9 by Material::BindSchemaDefaultTexture, which
         // is what makes an empty texture slot expressible at all.
         { "ShaderParam", "DefaultTexture", kMaterial, nullptr },

         // ---- ShaderRenderState: all fifteen land in the pipeline specification ----------------------
         { "ShaderRenderState", "Cull", kPipeline, nullptr },
         { "ShaderRenderState", "DepthTest", kPipeline, nullptr },
         { "ShaderRenderState", "DepthWrite", kPipeline, nullptr },
         { "ShaderRenderState", "DepthCompare", kPipeline, nullptr },
         { "ShaderRenderState", "Blend", kPipeline, nullptr },
         { "ShaderRenderState", "Topology", kPipeline, nullptr },
         { "ShaderRenderState", "PatchControlPoints", kPipeline, nullptr },
         { "ShaderRenderState", "BlendSrc", kPipeline, nullptr },
         { "ShaderRenderState", "BlendDst", kPipeline, nullptr },
         { "ShaderRenderState", "StencilTest", kPipeline, nullptr },
         { "ShaderRenderState", "StencilCompare", kPipeline, nullptr },
         { "ShaderRenderState", "StencilRef", kPipeline, nullptr },
         { "ShaderRenderState", "StencilFail", kPipeline, nullptr },
         { "ShaderRenderState", "StencilPass", kPipeline, nullptr },
         { "ShaderRenderState", "StencilDepthFail", kPipeline, nullptr },

         // ---- ShaderProgramMeta: the program-level schema -------------------------------------------
         { "ShaderProgramMeta", "Params", kParamRow, nullptr },
         { "ShaderProgramMeta", "State", kMeshRend, nullptr },
         { "ShaderProgramMeta", "Domain", kMeshRend, nullptr },
         { "ShaderProgramMeta", "PassNames", kShaderSvc, nullptr },
         // THE AUTHORED CLOUD MEDIUM's body — a program FRAGMENT, compiled INTO the four programs that
         // sample the cloud field rather than into one of its own (Docs/Clouds/O1_DESIGN.md §12).
         // ShaderService is the consumer: it recognises a medium at registration, keeps its text, and
         // hands it to the cloud renderer as the substitution for one virtual include.
         { "ShaderProgramMeta", "MediumSource", kShaderSvc, nullptr },

         // ---- The parser's own result ---------------------------------------------------------------

         // THE SECOND FINDING, and it is a two-things-that-must-agree with nothing asserting it. The DSL
         // opens with `Shader "Unlit"`, the parser reads that name and REFUSES an empty one — and the
         // engine's shader name is the FILE STEM (VulkanShader.cpp: `m_ShaderName =
         // m_ShaderPath.stem()`). So the declared name is validated and discarded: put `Shader "Foo"` in
         // `Bar.shader` and every `.demat` naming "Foo" finds nothing, with no message anywhere. All 76
         // shipped shaders agree today, and `TheDeclaredShaderNameIsTheFileStem` below is what keeps
         // them agreeing — a dead field turned into a checked invariant, which is the cheapest honest
         // thing to do with a name the runtime cannot use.
         { "DShaderParseResult", "Name", nullptr,
           "CHECKED, NOT CONSUMED: the runtime names a shader by its file stem, so this is validated and "
           "dropped. TheDeclaredShaderNameIsTheFileStem asserts the two agree; a runtime refusal would "
           "belong in VulkanShader, which М9 does not own." },

         { "DShaderParseResult", "Meta", kPreproc, nullptr },
         { "DShaderParseResult", "Stages", kPreproc, nullptr },
         { "DShaderParseResult", "Passes", kParser, nullptr },

         { "DShaderPass", "Name", kParser, nullptr },
         { "DShaderPass", "State", kPreproc, nullptr },
         { "DShaderPass", "Stages", kPreproc, nullptr },
    };

    // Where each struct is declared, so the field enumeration reads the same text the register describes.
    struct StructSource
    {
        const char* Struct;
        const char* Header;
    };

    constexpr StructSource k_Structs[] = {
         { "ShaderParam", "Desert/Desert/Source/Engine/Core/Formats/ShaderProgramMeta.hpp" },
         { "ShaderRenderState", "Desert/Desert/Source/Engine/Core/Formats/ShaderProgramMeta.hpp" },
         { "ShaderProgramMeta", "Desert/Desert/Source/Engine/Core/Formats/ShaderProgramMeta.hpp" },
         { "DShaderParseResult", "Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp" },
         { "DShaderPass", "Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp" },
    };

    std::string Key( const std::string& structName, const std::string& field )
    {
        return structName + "::" + field;
    }
} // namespace

// ------------------------------------------------------------------------------------------------
// Floors. Every assertion below is of the form "nothing unexpected was found", so a scan that found
// NOTHING passes all of them while checking nothing at all (contract §1.4).
// ------------------------------------------------------------------------------------------------

TEST( ShaderSchemaConsumers, TheScanSeesTheSchemaAtAll )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository from " << fs::current_path().string();

    for ( const auto& source : k_Structs )
    {
        const std::string body = StructBody( Strip( ReadAll( root + source.Header ) ), source.Struct );
        ASSERT_FALSE( body.empty() ) << source.Struct << " was not found in " << source.Header;

        const auto fields = DataMembers( body );
        ASSERT_GE( fields.size(), 3u ) << source.Struct << " yielded only " << fields.size()
                                       << " data members — the enumeration, not the schema, is what is wrong";
    }
}

TEST( ShaderSchemaConsumers, AnOperatorIsAFunctionNotAField )
{
    // ShaderRenderState gained `bool operator==( const ShaderRenderState& ) const = default;` and the
    // scan reported a field called `operator`: the `=` of the operator's name was read as an initialiser.
    const std::string              body   = "int Before = 1;\n"
                                            "bool operator==( const S& ) const = default;\n"
                                            "auto operator<=>( const S& ) const = default;\n"
                                            "S& operator=( const S& ) = default;\n"
                                            "std::optional<int> After;\n";
    const std::vector<std::string> fields = DataMembers( body );
    EXPECT_EQ( fields, ( std::vector<std::string>{ "Before", "After" } ) );
}

TEST( ShaderSchemaConsumers, EveryFieldOfTheSchemaIsInTheRegister )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::set<std::string> registered;
    for ( const auto& row : k_Census )
        registered.insert( Key( row.Struct, row.Field ) );

    for ( const auto& source : k_Structs )
    {
        const std::string body = StructBody( Strip( ReadAll( root + source.Header ) ), source.Struct );
        for ( const std::string& field : DataMembers( body ) )
        {
            EXPECT_TRUE( registered.count( Key( source.Struct, field ) ) != 0 )
                 << source.Struct << "::" << field
                 << " was added to the shader schema and no row of this census says who reads it. Add a "
                    "WIRED row naming the consumer file, or a DEAD row naming the reason and who decides.";
        }
    }
}

TEST( ShaderSchemaConsumers, NoRegisterRowNamesAFieldThatIsGone )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::set<std::string> present;
    for ( const auto& source : k_Structs )
    {
        const std::string body = StructBody( Strip( ReadAll( root + source.Header ) ), source.Struct );
        for ( const std::string& field : DataMembers( body ) )
            present.insert( Key( source.Struct, field ) );
    }

    for ( const auto& row : k_Census )
    {
        EXPECT_TRUE( present.count( Key( row.Struct, row.Field ) ) != 0 )
             << row.Struct << "::" << row.Field
             << " is in this census and no longer in the schema — a row nobody deleted with the field it "
                "describes is how a census starts certifying a structure that does not exist.";
    }
}

TEST( ShaderSchemaConsumers, EveryRowIsExactlyOneKind )
{
    for ( const auto& row : k_Census )
    {
        const bool wired = row.Where != nullptr;
        const bool dead  = row.Dead != nullptr;
        EXPECT_TRUE( wired != dead ) << row.Struct << "::" << row.Field
                                     << " must be either WIRED (a consumer file) or DEAD (a reason), "
                                        "never both and never neither";
    }
}

TEST( ShaderSchemaConsumers, EveryWiredRowsFileReallyReadsTheField )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const auto& row : k_Census )
    {
        if ( !row.Where )
            continue;

        const std::string raw = ReadAll( root + row.Where );
        ASSERT_FALSE( raw.empty() ) << row.Where << " could not be read (named by " << row.Struct
                                    << "::" << row.Field << ")";

        const std::string text = Strip( raw );

        // The file must SEE the schema, or a member read of a same-named field of some other type would
        // certify this row. That is the vacuity SettingConsumers measured and closed for components; this
        // is the strongest form of it available where the receiver's type is always `auto`.
        const bool seesSchema =
             text.find( "ShaderProgramMeta" ) != std::string::npos ||
             text.find( "GetProgramMeta" ) != std::string::npos || text.find( "GetSchema" ) != std::string::npos ||
             text.find( "DShaderParse" ) != std::string::npos || text.find( "ShaderParam" ) != std::string::npos ||
             text.find( "ShaderRenderState" ) != std::string::npos;
        EXPECT_TRUE( seesSchema ) << row.Where << " is named as the consumer of " << row.Struct
                                  << "::" << row.Field << " and does not name the schema at all";

        EXPECT_TRUE( ContainsMemberRead( text, row.Field ) )
             << row.Where << " no longer contains a member READ of " << row.Struct << "::" << row.Field
             << ". Either the consumer moved (point the row at the new file) or the field has become "
                "dead (say so, with the reason).";
    }
}

TEST( ShaderSchemaConsumers, EveryFieldIsActuallyFilledByTheParser )
{
    // The other half of the relation. A field with no reader is debt; a field with no WRITER either is a
    // different defect entirely, and a census that reported them as one thing would send the next person
    // looking for a consumer that was never the problem.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string parser = Strip( ReadAll( root + kParser ) );
    ASSERT_FALSE( parser.empty() ) << "the parser source could not be read";

    for ( const auto& row : k_Census )
    {
        EXPECT_TRUE( ContainsMemberTouch( parser, row.Field ) )
             << row.Struct << "::" << row.Field
             << " is in this census and DShaderParser.cpp never touches it. Either the parser stopped "
                "producing it (and every row about its consumers is now about nothing), or the field "
                "belongs to a structure this census should not be describing.";
    }
}

TEST( ShaderSchemaConsumers, TheDeadCountIsStatedSoAShrinkageIsVisible )
{
    // ONE, and it was THREE when М9 counted, then TWO. `ShaderParam::DefaultTexture` got its first reader
    // with М9; `ShaderParam::Category` got one with О9, which built the Material Editor's parameter groups
    // out of it — its row read "OWNER DECIDES / FENCED: 52 authored categories, and the panel draws one
    // flat table", true when written and stale ever since, and the ROW is what recorded that the fix
    // landed. `DShaderParseResult::Name` is what is left. Up is a regression; down is welcome, and this
    // line moves with it — a per-row diff never says "there are two more of these now".
    std::size_t dead = 0;
    for ( const auto& row : k_Census )
        dead += row.Dead != nullptr ? 1u : 0u;

    EXPECT_EQ( dead, 1u ) << "the number of shader-schema fields the parser fills and nothing reads has "
                             "changed";
    // FORTY-ONE since O1-E added `ShaderProgramMeta::MediumSource` — the authored cloud medium's body,
    // which is a program FRAGMENT rather than a program: ShaderService recognises it at registration and
    // hands its text to the cloud renderer as one virtual include. (Forty since O1 added
    // `ShaderParam::Timing`, when an edit to a parameter reaches the picture.)
    EXPECT_EQ( std::size( k_Census ), 41u )
         << "the shader schema gained or lost a field; the count is quoted so that is a reviewable edit";
}

// ------------------------------------------------------------------------------------------------
// The name -> pixel half of DefaultTextureKind. Two lists that must agree, which is the defect shape
// this repository keeps paying for, so it is asserted rather than reviewed.
// ------------------------------------------------------------------------------------------------

TEST( ShaderSchemaConsumers, EveryDefaultTextureKindHasAPixelAndASpelling )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string names  = ReadAll( root + "Desert/Desert/Source/Engine/Core/Formats/DefaultTexture.hpp" );
    const std::string pixels = ReadAll( root + "Desert/Desert/Source/Engine/Graphic/DefaultTextures.cpp" );
    ASSERT_FALSE( names.empty() );
    ASSERT_FALSE( pixels.empty() );

    // The enum's members, read out of its own declaration rather than typed here.
    const std::size_t open = names.find( "enum class DefaultTextureKind" );
    ASSERT_NE( open, std::string::npos );
    const std::size_t brace = names.find( '{', open );
    const std::size_t close = names.find( '}', brace );
    ASSERT_NE( close, std::string::npos );

    std::vector<std::string> kinds;
    std::string              ident;
    for ( std::size_t i = brace + 1; i < close; ++i )
    {
        if ( Desert::Tests::ConsumerText::IsIdentChar( names[i] ) )
        {
            ident += names[i];
            continue;
        }
        if ( !ident.empty() && std::isalpha( static_cast<unsigned char>( ident[0] ) ) != 0 )
            kinds.push_back( ident );
        ident.clear();
        if ( names[i] == '/' ) // skip the doc comment after each member
            while ( i < close && names[i] != '\n' )
                ++i;
    }
    ASSERT_GE( kinds.size(), 4u ) << "the enum scan found " << kinds.size() << " members";

    for ( const std::string& kind : kinds )
    {
        EXPECT_NE( pixels.find( "DefaultTextureKind::" + kind ), std::string::npos )
             << "DefaultTextureKind::" << kind
             << " has a name and no pixel: Graphic/DefaultTextures.cpp does not say what colour it is, so "
                "a slot defaulting to it would fall through to the switch's last resort.";
    }
}

// ------------------------------------------------------------------------------------------------
// Relations over the SHIPPED shaders. The register above is about the schema's fields; these two are
// about whether the values authored into the schema mean anything.
// ------------------------------------------------------------------------------------------------

namespace
{
    std::vector<fs::path> ShippedShaders( const std::string& root )
    {
        std::vector<fs::path> out;
        const fs::path        dir = fs::path( root ) / "Editor" / "Resources" / "Shaders";
        if ( !fs::exists( dir ) )
            return out;

        for ( const auto& entry : fs::recursive_directory_iterator( dir ) )
        {
            if ( entry.is_regular_file() && entry.path().extension() == ".shader" )
                out.push_back( entry.path() );
        }
        std::sort( out.begin(), out.end() );
        return out;
    }

    /// The quoted name of the leading `Shader "…"` declaration, or empty when there is none.
    std::string DeclaredShaderName( const std::string& source )
    {
        for ( std::size_t at = source.find( "Shader" ); at != std::string::npos;
              at             = source.find( "Shader", at + 1 ) )
        {
            if ( !CT::WordAt( source, at, "Shader" ) )
                continue;
            std::size_t i = CT::SkipSpace( source, at + 6 );
            if ( i >= source.size() || source[i] != '"' )
                continue;
            const std::size_t end = source.find( '"', i + 1 );
            if ( end == std::string::npos )
                continue;
            return source.substr( i + 1, end - i - 1 );
        }
        return {};
    }

    struct PropertiesBlock
    {
        bool        Found     = false;
        bool        Generated = false; ///< the block opted into TextureBinding(n): samplers are emitted
        std::string Body;              ///< the text between the braces
    };

    /// The `Properties [Binding(n)] [TextureBinding(m)] { … }` block of a shader source.
    ///
    /// FOUND BY SHAPE, NOT BY THE FIRST OCCURRENCE OF THE WORD. `Unlit.shader` opens with a comment that
    /// says "The Properties block both drives the Details UI and …", and a search for the word alone
    /// landed there, took the next `{` in the file (the Shader block's) and concluded the shader declared
    /// no TextureBinding — which reported a perfectly wired slot as dead. So the run between the keyword
    /// and the `{` must consist of nothing but the two option calls.
    PropertiesBlock FindPropertiesBlock( const std::string& source )
    {
        for ( std::size_t at = source.find( "Properties" ); at != std::string::npos;
              at             = source.find( "Properties", at + 1 ) )
        {
            if ( !CT::WordAt( source, at, "Properties" ) )
                continue;

            std::size_t i         = at + 10;
            bool        generated = false;
            bool        optionsOk = true;
            while ( i < source.size() )
            {
                i = CT::SkipSpace( source, i );
                if ( i < source.size() && source[i] == '{' )
                    break;

                const std::string option = CT::IdentAt( source, i );
                if ( option != "Binding" && option != "TextureBinding" )
                {
                    optionsOk = false;
                    break;
                }
                generated = generated || option == "TextureBinding";

                i = CT::SkipSpace( source, i + option.size() );
                const std::size_t end =
                     i < source.size() && source[i] == '(' ? source.find( ')', i ) : std::string::npos;
                if ( end == std::string::npos )
                {
                    optionsOk = false;
                    break;
                }
                i = end + 1;
            }

            if ( !optionsOk || i >= source.size() || source[i] != '{' )
                continue;

            int depth = 0;
            for ( std::size_t j = i; j < source.size(); ++j )
            {
                if ( source[j] == '{' )
                    ++depth;
                else if ( source[j] == '}' && --depth == 0 )
                    return { true, generated, source.substr( i + 1, j - i - 1 ) };
            }
        }
        return {};
    }
} // namespace

TEST( ShaderSchemaConsumers, TheDeclaredShaderNameIsTheFileStem )
{
    // The invariant that replaces the consumer `DShaderParseResult::Name` never got. The runtime names a
    // shader by its FILE, so a declaration that says anything else is a lie the engine cannot notice: a
    // `.demat` asking for the declared name would resolve to nothing and the mesh would silently draw
    // with the default PBR material. 76 files agree today; this is what keeps the 77th honest.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const auto shaders = ShippedShaders( root );
    ASSERT_GE( shaders.size(), 50u ) << "only " << shaders.size()
                                     << " .shader files were walked — the walk is what is wrong";

    for ( const auto& file : shaders )
    {
        const std::string declared = DeclaredShaderName( ReadAll( file ) );
        ASSERT_FALSE( declared.empty() ) << file.string() << " has no `Shader \"…\"` declaration";
        EXPECT_EQ( declared, file.stem().string() )
             << file.string() << " declares itself `Shader \"" << declared << "\"` and the engine will call it \""
             << file.stem().string()
             << "\" (VulkanShader takes the file stem). Every material naming the declared spelling "
                "resolves to nothing.";
    }
}

namespace
{
    /// A shader source together with everything it includes: each file comment- and literal-stripped,
    /// every `#include <path>` / `#include "path"` resolved as the shader compiler resolves it, recursively, each
    /// file once. The files are concatenated rather than spliced in place: the census asks whether a declaration
    /// EXISTS in what a stage compiles, not where.
    ///
    /// WHY THE CENSUS FOLLOWS INCLUDES. Terrain.shader offers u_GrassTex/u_RockTex/u_SnowTex and its
    /// fragment stage is a single `#include <Programs/Terrain/TerrainSurface.glslh>`, where the three
    /// `sampler2D` lines live. Reading only the `.shader` file reported three wired slots as dead; moving
    /// the declarations back to satisfy the census would have made the census the design authority.
    ///
    /// Directives are read from the RAW text, line by line: the stripper blanks what sits between the
    /// delimiters of an include, so the stripped text no longer names the file.
    struct ExpandedShader
    {
        std::string              Text;
        std::vector<std::string> Unresolved; ///< include targets the census could not open
    };

    void AppendExpanded( const fs::path& shadersDir, const fs::path& file, std::set<std::string>& visited,
                         ExpandedShader& out )
    {
        const std::string raw = ReadAll( file );
        out.Text += Strip( raw );
        out.Text += '\n';

        std::istringstream lines( raw );
        for ( std::string line; std::getline( lines, line ); )
        {
            const std::size_t hash = CT::SkipSpace( line, 0 );
            if ( line.compare( hash, 8, "#include" ) != 0 )
                continue;
            const std::size_t open = CT::SkipSpace( line, hash + 8 );
            if ( open >= line.size() || ( line[open] != '<' && line[open] != '"' ) )
            {
                out.Unresolved.push_back( line );
                continue;
            }
            const std::size_t close = line.find( line[open] == '<' ? '>' : '"', open + 1 );
            if ( close == std::string::npos )
            {
                out.Unresolved.push_back( line );
                continue;
            }
            // `<path>` names a file under the Shaders directory; `"path"` is looked up next to the including
            // file first (Mesh/PointLight.glslh includes "DirectLighting.glslh"), as a relative include is.
            const std::string target   = line.substr( open + 1, close - open - 1 );
            fs::path          included = shadersDir / target;
            if ( line[open] == '"' && fs::exists( file.parent_path() / target ) )
                included = file.parent_path() / target;
            if ( !fs::exists( included ) )
            {
                out.Unresolved.push_back( target );
                continue;
            }
            if ( !visited.insert( fs::weakly_canonical( included ).string() ).second )
                continue;
            AppendExpanded( shadersDir, included, visited, out );
        }
    }

    ExpandedShader ExpandIncludes( const fs::path& shadersDir, const fs::path& file )
    {
        ExpandedShader        out;
        std::set<std::string> visited;
        AppendExpanded( shadersDir, file, visited, out );
        return out;
    }
} // namespace

TEST( ShaderSchemaConsumers, TheSamplerSearchFollowsIncludes )
{
    // THE RELATION the sampler census depends on, pinned on the shader that needed it: Terrain.shader's
    // own text declares none of its three texture samplers, and its expansion declares all three.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const fs::path shadersDir = fs::path( root ) / "Editor" / "Resources" / "Shaders";
    const fs::path terrain    = shadersDir / "Programs" / "Terrain" / "Terrain.shader";
    ASSERT_TRUE( fs::exists( terrain ) );

    const std::string    own      = Strip( ReadAll( terrain ) );
    const ExpandedShader expanded = ExpandIncludes( shadersDir, terrain );
    EXPECT_TRUE( expanded.Unresolved.empty() ) << "first unresolved include: " << expanded.Unresolved.front();
    for ( const char* name : { "u_GrassTex", "u_RockTex", "u_SnowTex" } )
    {
        EXPECT_EQ( own.find( std::string( "sampler2D " ) + name ), std::string::npos )
             << name << " is declared in Terrain.shader itself again; this test no longer exercises includes";
        EXPECT_NE( expanded.Text.find( std::string( "sampler2D " ) + name ), std::string::npos )
             << name << " is not reached through Terrain.shader's includes";
    }
}

TEST( ShaderSchemaConsumers, EveryTexturePropertyHasASamplerToBindTo )
{
    // A `Texture2D`/`TextureCube` line in a Properties block is a SLOT IN THE MATERIAL EDITOR: it gets a
    // row, an artist can drop a texture on it, and the value is persisted into the `.demat`. If the
    // shader has no sampler of that name, all of that happens and nothing is ever sampled — a dead
    // setting by DC §1.3, wearing a working control's clothes.
    //
    // A block written `Properties … TextureBinding(n)` has its samplers GENERATED by the parser, so the
    // relation holds by construction there. A metadata-only block declares them by hand, and that is
    // where the two halves can drift.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // The known drift, with the reason and the owner. StaticMeshPBR's schema offers seven texture slots
    // and the shader samples three; the four below are persisted by `PBRSurfaceParams` (the mesh importer
    // fills them from FBX/glTF) and read by no stage of any PBR shader. Deleting them throws away import
    // data the engine may want; wiring them is a shading-model change. Either way it is not a tidy-up,
    // and М9 found it rather than owning it.
    static const std::set<std::string> knownUnsampled = { "u_MetallicTexture", "u_RoughnessTexture", "u_AOTexture",
                                                          "u_EmissiveTexture" };
    std::set<std::string>              seenUnsampled;

    const fs::path shadersDir = fs::path( root ) / "Editor" / "Resources" / "Shaders";
    for ( const auto& file : ShippedShaders( root ) )
    {
        // Comments stripped: a `.shader` explains its own bindings in prose, and "declares no sampler2D"
        // must not be answered by a sentence that mentions one. Includes followed: a stage that is one
        // `#include` declares its samplers in the included file.
        const ExpandedShader expanded = ExpandIncludes( shadersDir, file );
        for ( const std::string& hole : expanded.Unresolved )
            ADD_FAILURE() << file.string() << ": the census cannot follow `" << hole
                          << "`, so a sampler declared behind it would be reported missing";
        const std::string& source = expanded.Text;

        const PropertiesBlock properties = FindPropertiesBlock( source );
        if ( !properties.Found || properties.Generated )
            continue;

        const std::string& block = properties.Body;

        for ( const char* keyword : { "Texture2D", "TextureCube" } )
        {
            const std::string sampler = std::string( keyword ) == "Texture2D" ? "sampler2D" : "samplerCube";
            for ( std::size_t at = block.find( keyword ); at != std::string::npos;
                  at             = block.find( keyword, at + 1 ) )
            {
                if ( !CT::WordAt( block, at, keyword ) )
                    continue;

                const std::string name =
                     CT::IdentAt( block, CT::SkipSpace( block, at + std::string( keyword ).size() ) );
                if ( name.empty() )
                    continue;

                const bool declared = source.find( sampler + " " + name ) != std::string::npos;
                if ( declared )
                    continue;

                if ( knownUnsampled.count( name ) != 0 )
                {
                    seenUnsampled.insert( name );
                    continue;
                }

                ADD_FAILURE() << file.string() << " offers a " << keyword << " slot '" << name
                              << "' in its Properties block and declares no `" << sampler << ' ' << name
                              << "` in any stage. The Material Editor will draw that slot, an artist can "
                                 "fill it, the .demat will carry it and nothing will ever sample it.";
            }
        }
    }

    EXPECT_EQ( seenUnsampled, knownUnsampled )
         << "the known-unsampled list and what the shaders actually declare have drifted apart — a slot "
            "that got its sampler must leave this list, or the list is describing a repair as debt";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
