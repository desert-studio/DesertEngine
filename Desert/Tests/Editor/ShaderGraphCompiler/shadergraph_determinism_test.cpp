// THE EMITTER MUST PRODUCE ONE TEXT FOR ONE GRAPH — ON EVERY MACHINE, IN EVERY RUN.
//
// ── WHAT THIS SUITE IS PAYING FOR ────────────────────────────────────────────────────────────────────
//
// `dev` went red on Windows while macOS, ASan and the format gate were all green, on exactly one test:
// the Volume domain's "pin i reads component .xyzw[i] of the fetch". The Windows emitter had written
//
//     vec4  n1 = CLOUD_SAMPLE_NOISE( field.NoiseSlot, CloudDefaultNoiseCoordinate( params, positionKm ) );
//     float n2 = CloudDefaultDensity( params, field, positionKm );
//     float n0 = n2 * n1.x;
//
// where macOS writes the same three statements with n1 and n2 exchanged. Both are correct GLSL and both
// compute the same number, so nothing about the PICTURE was wrong. What was wrong is that there were two
// of them.
//
// The mechanism is one line of C++ and it is not the unordered containers the traversal is full of
// (those are only ever probed by key — measured, not assumed; see AnAddressPermutationDoesNotMoveOneByte
// below). Resolving an input EMITS the subgraph behind it, and thirteen emitter branches asked for two
// or three inputs inside ONE `std::format(...)` argument list. **C++ does not order function
// arguments.** clang evaluates them left to right, MSVC right to left, and both are conforming. So the
// order in which the graph was walked — and therefore every variable's number and every statement's
// position — was a property of the compiler that built the editor.
//
// ── WHY THAT IS SERIOUS AND NOT COSMETIC ─────────────────────────────────────────────────────────────
//
// The emitted text is not an intermediate. It is written to a committed `.shader`
// (NodeGraphPanel::Compile), and for a cloud medium it is carried as the bytes of a virtual include:
// `Core::ShaderVariant::Hash()` is an FNV-1a over exactly those bytes, `ComputeShaderCacheKey` mixes
// that hash, and `Graphic::CloudEnvironmentFingerprint` mixes it again. Two compilers therefore give one
// graph two SPIR-V cache identities and two cloud-environment fingerprints. Every byte-exact claim made
// about generated shader text before Г24 is a claim about macOS.
//
// ── WHAT IS ASSERTED, AND WHY IT TAKES THREE DIFFERENT SHAPES ────────────────────────────────────────
//
// No test that RUNS the emitter on this machine can see an unspecified-evaluation-order defect, because
// on this machine the order clang picks happens to be the intended one. That is the whole difficulty of
// the bug and it is why one assertion is not enough:
//
//   1. AtMostOneEmittingCallPerFullExpression — a census over the emitter's own TEXT. This is the one
//      that would have gone red on macOS before the Windows job ever started, and it is red for the
//      SHAPE rather than for a symptom, so the fourteenth branch is covered the day it is written.
//   2. EveryMultiInputNodeEmitsItsInputsInPinOrder — derived from the catalogue, not listed: every node
//      kind that reads two or more inputs is wired up and compiled, and the subgraph behind pin i must
//      appear before the subgraph behind pin i+1. This is the OUTCOME the census protects, and it does
//      go red on macOS for the other way of getting the order wrong — writing the resolutions out in
//      the wrong sequence by hand.
//   3. AnAddressPermutationDoesNotMoveOneByte / LinkOrderIsNotPartOfTheText /
//      AMediumsPropertiesAreInDocumentOrderNotTraversalOrder — the sibling hazard, and the one the first
//      hypothesis blamed: iterating an `unordered_map`/`unordered_set` keyed by `const Node*`. That one
//      IS observable here, because the addresses move between two documents. It was measured and the
//      hypothesis was WRONG — every one of those containers is probed by key, never walked — but the
//      third of these tests also closes something that was only a comment until now: the medium's
//      `Properties` block IS the GPU packing order, and nothing asserted it was in document order.

#include <gtest/gtest.h>

#include "../../Engine/SettingConsumers/setting_consumers_reader.hpp"
#include "graph_test_tree.hpp"

#include <ShaderGraph.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace SG = Desert::Editor::ShaderGraph;

using Desert::Tests::ShaderGraph::ReadAll;
using Desert::Tests::ShaderGraph::RepoRoot;

namespace
{
    std::filesystem::path EmitterSource()
    {
        return RepoRoot() / "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp";
    }

    /// The emitter's input resolver. Named here once so a rename reddens this suite (the count assertion
    /// below) instead of silently emptying the census.
    constexpr const char* kEmittingCall = "EmitInput";

    /// Code with comments and string literals blanked to spaces, LENGTH PRESERVED. The blanker is shared
    /// with the settings census rather than written a second time; blanking rather than deleting matters
    /// because a `;` inside a format string ("float {} = {} * {};") would otherwise cut a statement in
    /// half and the census would read two halves as two statements.
    std::vector<std::string> FullExpressionsOf( const std::string& source )
    {
        const std::string        code = Desert::Tests::ConsumerText::StripCommentsAndLiterals( source );
        std::vector<std::string> out;

        std::string::size_type from = 0;
        for ( std::string::size_type at = code.find( ';' ); at != std::string::npos; at = code.find( ';', from ) )
        {
            out.push_back( code.substr( from, at - from ) );
            from = at + 1;
        }
        out.push_back( code.substr( from ) );
        return out;
    }

    /// Whole-word occurrences of @p name that are CALLS — the identifier followed by an open paren, so a
    /// mention in a member list or a declaration of a different symbol with the same prefix is not one.
    int CountCalls( const std::string& text, const char* name )
    {
        int seen = 0;
        for ( const std::string::size_type at : Desert::Tests::ConsumerText::WordPositions( text, name ) )
        {
            std::string::size_type after = at + std::string( name ).size();
            while ( after < text.size() && text[after] == ' ' )
                ++after;
            if ( after < text.size() && text[after] == '(' )
                ++seen;
        }
        return seen;
    }

    // ---------------------------------------------------------------- graph builders --------------
    const SG::NodeSpec& SpecOf( const std::string& kind )
    {
        const SG::NodeSpec* spec = SG::FindSpec( kind );
        EXPECT_NE( spec, nullptr ) << "the catalogue has no node kind '" << kind << "'";
        return *spec;
    }

    /// A node kind that PRODUCES @p type and emits a statement of its own, so its position in the body is
    /// observable. One per type; `nullptr` when the palette has no such producer.
    const char* ProducerOf( SG::ValueType type )
    {
        switch ( type )
        {
            case SG::ValueType::Float:
                return "FloatConst";
            case SG::ValueType::Color:
                return "ColorConst";
            case SG::ValueType::Vec3:
                return "Vec3Const";
            case SG::ValueType::Vec2:
                return "UV";
        }
        return nullptr;
    }

    /// The text a producer built by @p MakeProducer for input index @p index writes into the body. The
    /// index is baked into the literal so two inputs of the SAME type are still told apart.
    std::string ProducerMark( SG::ValueType type, size_t index )
    {
        const std::string n = std::to_string( 101 + index ) + ".0";
        switch ( type )
        {
            case SG::ValueType::Float:
                return "= " + n + ";";
            case SG::ValueType::Color:
                return "vec4( " + n + ",";
            case SG::ValueType::Vec3:
                return "vec3( " + n + ",";
            case SG::ValueType::Vec2:
                return "= v_UV;";
        }
        return {};
    }

    /// Feeds every input of a node of kind @p kind, wires its first output into the domain's output node,
    /// and returns the document — or an empty optional-shaped `Nodes` when this domain cannot sink the
    /// node's output type. @p marks receives, in pin order, the text each input's producer emits.
    bool BuildProbe( SG::Domain domain, const std::string& kind, SG::Document& doc,
                     std::vector<std::string>& marks )
    {
        const SG::NodeSpec& spec = SpecOf( kind );
        if ( !SG::SpecInDomain( spec, domain ) )
            return false;

        doc        = SG::Document{};
        doc.Domain = static_cast<int>( domain );
        doc.Name   = "DeterminismProbe";
        // The Particles nodes are refused in a material without the sprite usage; the probe is about pin order.
        doc.UsedWithParticleSprites = SG::ReadsParticleInputs( kind );
        marks.clear();

        SG::Node       outputNode = SG::MakeNode( doc, SG::OutputKind( domain ) );
        const SG::Node outCopy    = outputNode;
        doc.Nodes.push_back( std::move( outputNode ) );

        // A sink of the node's own output type. Not every type has one in every domain (a Volume medium
        // has no vec4 sink at all), and that is a legitimate skip rather than a failure — О1-I's own gate
        // is the one that polices "a type with no receiver in this domain".
        const auto sinkFor = [&outCopy]( SG::ValueType type )
        {
            for ( size_t i = 0; i < outCopy.Inputs.size(); ++i )
                if ( static_cast<SG::ValueType>( outCopy.Inputs[i].Type ) == type )
                    return i;
            return outCopy.Inputs.size();
        };

        SG::Node       under    = SG::MakeNode( doc, kind );
        const SG::Node underCpy = under;
        doc.Nodes.push_back( std::move( under ) );

        // The node under test does not have to reach the output DIRECTLY. TileUV produces a vec2 and no
        // domain output has a vec2 pin, so without one adapter — a TextureSample, whose UV input is
        // exactly that vec2 — the only node in the catalogue with a Vec2 input would be the one kind this
        // suite never exercised. The adapter's own other inputs stay unlinked and emit their fallbacks
        // inline, so it adds no statement that could be confused with a producer's.
        const SG::ValueType produced = spec.Outputs.front().Type;
        uint64_t            tail     = underCpy.Outputs.front().Id;
        size_t              sink     = sinkFor( produced );
        if ( sink == outCopy.Inputs.size() )
        {
            for ( const SG::NodeSpec& adapter : SG::Specs() )
            {
                if ( adapter.Kind == kind || adapter.Inputs.empty() || adapter.Outputs.empty() )
                    continue;
                if ( adapter.Inputs.front().Type != produced || !SG::SpecInDomain( adapter, domain ) )
                    continue;
                const size_t adapted = sinkFor( adapter.Outputs.front().Type );
                if ( adapted == outCopy.Inputs.size() )
                    continue;

                SG::Node       bridge    = SG::MakeNode( doc, adapter.Kind );
                const SG::Node bridgeCpy = bridge;
                doc.Nodes.push_back( std::move( bridge ) );
                doc.Links.push_back( { doc.NextId++, tail, bridgeCpy.Inputs.front().Id } );
                tail = bridgeCpy.Outputs.front().Id;
                sink = adapted;
                break;
            }
        }
        if ( sink == outCopy.Inputs.size() )
            return false;

        for ( size_t i = 0; i < spec.Inputs.size(); ++i )
        {
            const SG::ValueType type     = spec.Inputs[i].Type;
            const char*         producer = ProducerOf( type );
            if ( !producer || !SG::SpecInDomain( SpecOf( producer ), domain ) )
                return false;

            SG::Node source    = SG::MakeNode( doc, producer );
            source.Value       = { 101.0f + static_cast<float>( i ), 0.0f, 0.0f, 0.0f };
            const uint64_t out = source.Outputs.front().Id;
            doc.Nodes.push_back( std::move( source ) );
            doc.Links.push_back( { doc.NextId++, out, underCpy.Inputs[i].Id } );
            const std::string mark = ProducerMark( type, i );
            // TWO PINS THAT LOOK THE SAME IN THE TEXT WOULD MAKE THIS TEST LIE — both `find` calls would
            // land on the same statement and the order check would pass on one line. Refused as
            // "uncovered" instead, which names the kind out loud.
            if ( mark.empty() || std::find( marks.begin(), marks.end(), mark ) != marks.end() )
                return false;
            marks.push_back( mark );
        }

        doc.Links.push_back( { doc.NextId++, tail, outCopy.Inputs[sink].Id } );
        return true;
    }

    /// A document with something of every shape in it — two operands of one type, a three-input mix, a
    /// texture sample, a parameter and a constant — used by the two invariance tests below.
    SG::Document MixedSurfaceDoc()
    {
        SG::Document doc;
        doc.Name   = "MixedProbe";
        doc.Domain = static_cast<int>( SG::Domain::Surface );

        SG::Node       output = SG::MakeNode( doc, "SurfaceOutput" );
        const SG::Node outCpy = output;
        doc.Nodes.push_back( std::move( output ) );

        SG::Node       lerp    = SG::MakeNode( doc, "Lerp" );
        const SG::Node lerpCpy = lerp;
        doc.Nodes.push_back( std::move( lerp ) );

        SG::Node tex          = SG::MakeNode( doc, "TextureSample" );
        tex.ParamName         = "u_Albedo";
        const SG::Node texCpy = tex;
        doc.Nodes.push_back( std::move( tex ) );

        SG::Node tint          = SG::MakeNode( doc, "ColorParam" );
        tint.ParamName         = "Tint";
        const SG::Node tintCpy = tint;
        doc.Nodes.push_back( std::move( tint ) );

        SG::Node amount          = SG::MakeNode( doc, "FloatConst" );
        amount.Value             = { 0.25f, 0.0f, 0.0f, 0.0f };
        const SG::Node amountCpy = amount;
        doc.Nodes.push_back( std::move( amount ) );

        SG::Node       uv    = SG::MakeNode( doc, "TileUV" );
        const SG::Node uvCpy = uv;
        doc.Nodes.push_back( std::move( uv ) );

        SG::Node tiling          = SG::MakeNode( doc, "FloatConst" );
        tiling.Value             = { 4.0f, 0.0f, 0.0f, 0.0f };
        const SG::Node tilingCpy = tiling;
        doc.Nodes.push_back( std::move( tiling ) );

        doc.Links.push_back( { doc.NextId++, tilingCpy.Outputs[0].Id, uvCpy.Inputs[1].Id } );
        doc.Links.push_back( { doc.NextId++, uvCpy.Outputs[0].Id, texCpy.Inputs[0].Id } );
        doc.Links.push_back( { doc.NextId++, texCpy.Outputs[0].Id, lerpCpy.Inputs[0].Id } );
        doc.Links.push_back( { doc.NextId++, tintCpy.Outputs[0].Id, lerpCpy.Inputs[1].Id } );
        doc.Links.push_back( { doc.NextId++, amountCpy.Outputs[0].Id, lerpCpy.Inputs[2].Id } );
        doc.Links.push_back( { doc.NextId++, lerpCpy.Outputs[0].Id, outCpy.Inputs[0].Id } );
        return doc;
    }

    /// The names of a medium's four exposed properties, IN DOCUMENT ORDER. `MixedVolumeDoc` wires them so
    /// the five traversals reach them in exactly the reverse of this.
    const std::vector<std::string>& MediumPropertyOrder()
    {
        static const std::vector<std::string> order = { "Alpha", "Bravo", "Charlie", "Delta" };
        return order;
    }

    /// A medium with four exposed properties whose DOCUMENT order is the exact reverse of the order the
    /// five output traversals reach them in.
    ///
    /// The difference is the whole point. A Volume graph's `Properties` block is written in document
    /// order, and the emitter says out loud why: the runtime packs one vec4 per property in exactly that
    /// order, so a block ordered by which traversal happened to arrive first would make the GPU layout
    /// depend on which output pin the artist wired first. The set that answers "was this node reached" is
    /// an `unordered_set<const Node*>` — iterating it instead of the document is a one-line edit that
    /// nothing but this fixture can see, and a fixture whose two orders agree would call that edit
    /// correct.
    SG::Document MixedVolumeDoc()
    {
        SG::Document doc;
        doc.Name   = "MixedMedium";
        doc.Domain = static_cast<int>( SG::Domain::Volume );

        SG::Node       output = SG::MakeNode( doc, "VolumeOutput" );
        const SG::Node outCpy = output;
        doc.Nodes.push_back( std::move( output ) );

        const auto pinNamed = [&outCpy]( const char* name )
        {
            for ( size_t i = 0; i < outCpy.Inputs.size(); ++i )
                if ( outCpy.Inputs[i].Name == name )
                    return i;
            return outCpy.Inputs.size();
        };
        const size_t density    = pinNamed( "Density" );
        const size_t extinction = pinNamed( "Extinction" );
        EXPECT_NE( density, outCpy.Inputs.size() );
        EXPECT_NE( extinction, outCpy.Inputs.size() );

        std::vector<uint64_t> outs;
        for ( const std::string& name : MediumPropertyOrder() )
        {
            SG::Node property  = SG::MakeNode( doc, "FloatParam" );
            property.ParamName = name;
            outs.push_back( property.Outputs[0].Id );
            doc.Nodes.push_back( std::move( property ) );
        }

        // Delta + Charlie into Density, Bravo + Alpha into Extinction: the medium's Density function is
        // compiled first and reaches Delta first, so the traversal order is D, C, B, A.
        const auto sum = [&doc]( uint64_t first, uint64_t second, uint64_t into )
        {
            SG::Node       add    = SG::MakeNode( doc, "AddFloat" );
            const SG::Node addCpy = add;
            doc.Nodes.push_back( std::move( add ) );
            doc.Links.push_back( { doc.NextId++, first, addCpy.Inputs[0].Id } );
            doc.Links.push_back( { doc.NextId++, second, addCpy.Inputs[1].Id } );
            doc.Links.push_back( { doc.NextId++, addCpy.Outputs[0].Id, into } );
        };
        sum( outs[3], outs[2], outCpy.Inputs[density].Id );
        sum( outs[1], outs[0], outCpy.Inputs[extinction].Id );
        return doc;
    }

    std::vector<SG::Document> InvarianceCorpus()
    {
        return { MixedSurfaceDoc(), MixedVolumeDoc() };
    }
} // namespace

// ================================================================= 1. the shape ==================

TEST( ShaderGraphDeterminism, AtMostOneEmittingCallPerFullExpression )
{
    const std::string source = ReadAll( EmitterSource() );
    ASSERT_FALSE( source.empty() ) << "the emitter is not where this census looks: " << EmitterSource();

    int total = 0;
    for ( const std::string& statement : FullExpressionsOf( source ) )
    {
        const int calls = CountCalls( statement, kEmittingCall );
        total += calls;
        EXPECT_LE( calls, 1 )
             << "this full expression resolves " << calls << " inputs at once:\n"
             << statement
             << "\n\nResolving an input EMITS the subgraph behind it — it advances nextVar and appends to "
                "body. C++ leaves the order of function arguments unspecified, so two of these in one "
                "expression is a graph walked left-to-right under clang and right-to-left under MSVC, and "
                "the same .dgraph then compiles to two different .shader files. Resolve them into named "
                "locals first, in pin order: consecutive statements ARE sequenced.";
    }

    // NOT VACUOUS. A rename of the resolver, or an emitter that stopped resolving inputs at all, would
    // satisfy every assertion above by having nothing to check.
    EXPECT_GE( total, 20 ) << "the census found only " << total << " call(s) to " << kEmittingCall
                           << " in the emitter. Either the function was renamed — update kEmittingCall —"
                              " or this census is now measuring nothing.";
}

// ================================================================= 2. the outcome ================

TEST( ShaderGraphDeterminism, EveryMultiInputNodeEmitsItsInputsInPinOrder )
{
    // DERIVED FROM THE CATALOGUE, NOT LISTED. The Windows failure was found by one test that happened to
    // wire a MultiplyFloat; the property it was really about holds for every node the emitter builds out
    // of more than one input, and a hand-written list would have covered twelve of the thirteen.
    //
    // The domain OUTPUT nodes are excluded and it is not an omission: they are not compiled into one
    // declaration. A Surface Output emits one statement per pin and a Volume Output compiles five
    // SEPARATE functions in the order of its own function table, so "pin i before pin i+1" is not the
    // rule there and asserting it would pin the wrong thing.
    const std::vector<SG::Domain> domains = { SG::Domain::Surface, SG::Domain::PostProcess, SG::Domain::Volume };

    std::vector<std::string> uncovered;
    int                      covered = 0;

    for ( const SG::NodeSpec& spec : SG::Specs() )
    {
        if ( spec.Inputs.size() < 2 || spec.Outputs.empty() )
            continue;
        const std::string kind = spec.Kind;
        if ( kind == SG::OutputKind( SG::Domain::Surface ) || kind == SG::OutputKind( SG::Domain::PostProcess ) ||
             kind == SG::OutputKind( SG::Domain::Volume ) )
            continue;

        bool probed = false;
        for ( const SG::Domain domain : domains )
        {
            SG::Document             doc;
            std::vector<std::string> marks;
            if ( !BuildProbe( domain, kind, doc, marks ) )
                continue;

            const auto compiled = SG::CompileToDShader( doc );
            ASSERT_TRUE( compiled.IsSuccess() )
                 << kind << " in domain " << static_cast<int>( domain ) << ": " << compiled.GetError();
            const std::string& text = compiled.GetValue();

            std::string::size_type previous = 0;
            for ( size_t i = 0; i < marks.size(); ++i )
            {
                const std::string::size_type at = text.find( marks[i] );
                ASSERT_NE( at, std::string::npos )
                     << kind << ": the producer feeding pin " << i << " ('" << spec.Inputs[i].Name
                     << "') was not emitted at all. Looked for '" << marks[i] << "' in:\n"
                     << text;
                if ( i > 0 )
                    EXPECT_GT( at, previous )
                         << kind << ": the subgraph behind pin " << i << " ('" << spec.Inputs[i].Name
                         << "') is emitted BEFORE the one behind pin " << ( i - 1 ) << " ('"
                         << spec.Inputs[i - 1].Name
                         << "'). The emitter walked this node's inputs in the wrong order, so its variable "
                            "numbering — and the whole text below it — is not the one every other machine "
                            "produces:\n"
                         << text;
                previous = at;
            }
            probed = true;
            ++covered;
            break;
        }

        if ( !probed )
            uncovered.push_back( kind );
    }

    // A REGISTER, NOT A COUNT: every multi-input kind must be named here or exercised above, so a kind
    // that becomes unreachable (no producer for its pin type, no sink for its output) is a finding rather
    // than a silent hole in the sweep.
    std::string uncoveredNames;
    for ( const std::string& kind : uncovered )
        uncoveredNames += ( uncoveredNames.empty() ? "" : ", " ) + kind;

    EXPECT_TRUE( uncovered.empty() )
         << "these multi-input node kinds could not be wired up in any domain, so nothing above checked "
            "their emission order: "
         << uncoveredNames;

    EXPECT_GE( covered, 14 ) << "only " << covered
                             << " multi-input node kind(s) were exercised; the catalogue offered fourteen "
                                "when this was written, so this test is measuring less than it claims to";
}

// ================================================================= 3. the sibling hazard =========

TEST( ShaderGraphDeterminism, AnAddressPermutationDoesNotMoveOneByte )
{
    // THE FIRST HYPOTHESIS, TESTED RATHER THAN BELIEVED. The compiler keeps five unordered containers
    // keyed by `const Node*` or by pin id. If any of them were ever ITERATED, the emitted text would
    // depend on where the document happens to sit in memory — a defect that needs no second platform and
    // that a single-process run can see, because two documents never share addresses.
    //
    // It is green today (every one of those containers is probed by key), and that is the measured
    // disproof of the hypothesis the Windows failure was first blamed on. It stays because the day
    // somebody writes `for ( const Node* n : touchedParams )` this goes red on this machine.
    for ( const SG::Document& reference : InvarianceCorpus() )
    {
        const auto expected = SG::CompileToDShader( reference );
        ASSERT_TRUE( expected.IsSuccess() ) << reference.Name << ": " << expected.GetError();

        // EVERY COPY IS KEPT ALIVE, and that is what makes the addresses genuinely new. A copy taken and
        // dropped in a loop gets the same recycled block back nearly every time, and a probe of libc++
        // measured the consequence: `unordered_set<const T*>` over four pointers yields ONE iteration
        // order across six recycled addresses and two across six distinct ones. A version of this test
        // that let its documents die was therefore green under a deliberately pointer-ordered emitter —
        // an equivalent mutation, not a passing one.
        std::vector<std::unique_ptr<SG::Document>> alive;
        std::vector<std::unique_ptr<char[]>>       ballast;
        for ( int attempt = 0; attempt < 24; ++attempt )
        {
            for ( int i = 0; i < attempt % 7 + 1; ++i )
                ballast.push_back( std::make_unique<char[]>( 97u * static_cast<unsigned>( attempt + i + 1 ) ) );

            alive.push_back( std::make_unique<SG::Document>( reference ) );
            const auto again = SG::CompileToDShader( *alive.back() );
            ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
            EXPECT_EQ( again.GetValue(), expected.GetValue() )
                 << reference.Name
                 << " compiled to different text at a different address, so something in the emitter "
                    "iterates a container keyed by pointer";
        }
    }
}

TEST( ShaderGraphDeterminism, AMediumsPropertiesAreInDocumentOrderNotTraversalOrder )
{
    // THE ONE CLAIM IN THIS EMITTER THAT WAS ONLY A COMMENT. The Volume path says "IN DOCUMENT ORDER, NOT
    // IN THE ORDER THE FIVE TRAVERSALS HAPPENED TO REACH THEM ... the order here IS the layout", and the
    // runtime really does pack one vec4 per property in the order this block declares
    // (Graphic::BuildCloudMediumValues). Nothing asserted it. Changing the loop to walk `touched` — the
    // `unordered_set<const Node*>` right beside it — compiles, passes every other test in this suite, and
    // silently makes a medium's GPU layout depend on which output pin the artist wired first.
    const auto compiled = SG::CompileToDShader( MixedVolumeDoc() );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const std::string& text = compiled.GetValue();

    std::string::size_type previous = 0;
    for ( size_t i = 0; i < MediumPropertyOrder().size(); ++i )
    {
        const std::string            declaration = "Float     " + MediumPropertyOrder()[i] + " (";
        const std::string::size_type at          = text.find( declaration );
        ASSERT_NE( at, std::string::npos )
             << "the medium does not declare '" << MediumPropertyOrder()[i] << "' at all:\n"
             << text;
        if ( i > 0 )
            EXPECT_GT( at, previous ) << "the Properties block declares '" << MediumPropertyOrder()[i]
                                      << "' before '" << MediumPropertyOrder()[i - 1]
                                      << "', but the document declares them the other way round. This "
                                         "block IS the GPU layout — the order here is the order the "
                                         "runtime packs the vec4s in:\n"
                                      << text;
        previous = at;
    }
}

TEST( ShaderGraphDeterminism, LinkOrderIsNotPartOfTheText )
{
    // A .dgraph stores its links in the order the artist happened to draw them, and redrawing one link is
    // an edit that changes nothing about what the graph MEANS. If the text moved with that order, every
    // such edit would be a new shader cache key and a new cloud fingerprint for an unchanged material.
    for ( const SG::Document& reference : InvarianceCorpus() )
    {
        const auto expected = SG::CompileToDShader( reference );
        ASSERT_TRUE( expected.IsSuccess() ) << reference.Name << ": " << expected.GetError();

        SG::Document reversed = reference;
        std::reverse( reversed.Links.begin(), reversed.Links.end() );
        const auto again = SG::CompileToDShader( reversed );
        ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
        EXPECT_EQ( again.GetValue(), expected.GetValue() )
             << reference.Name << ": reversing the order of the Links vector changed the emitted text";
    }
}

TEST( ShaderGraphDeterminism, EveryCommittedGraphCompilesToTheSameTextTwice )
{
    // The corpus, not a fixture: whatever the shipped graphs contain is what the shader cache and the
    // cloud fingerprint are computed over.
    const std::filesystem::path directory = RepoRoot() / "Projects/Desert/Content/ShaderGraphs";
    ASSERT_TRUE( std::filesystem::is_directory( directory ) ) << directory;

    int seen = 0;
    for ( const auto& entry : std::filesystem::directory_iterator( directory ) )
    {
        if ( entry.path().extension() != ".dgraph" )
            continue;

        const auto loaded = SG::Deserialize( ReadAll( entry.path() ) );
        ASSERT_TRUE( loaded.IsSuccess() ) << entry.path().string() << ": " << loaded.GetError();

        const auto first = SG::CompileToDShader( loaded.GetValue().Doc );
        if ( !first.IsSuccess() )
            continue; // the corpus gate next door owns "does it compile"; this one owns "is it stable"

        const auto second = SG::Deserialize( ReadAll( entry.path() ) );
        ASSERT_TRUE( second.IsSuccess() );
        SG::Document shuffled = second.GetValue().Doc;
        std::reverse( shuffled.Links.begin(), shuffled.Links.end() );

        const auto again = SG::CompileToDShader( shuffled );
        ASSERT_TRUE( again.IsSuccess() ) << entry.path().string() << ": " << again.GetError();
        EXPECT_EQ( again.GetValue(), first.GetValue() )
             << entry.path().filename().string() << " does not compile to one text";

        // PRINTED, NOT PINNED, and the distinction is deliberate. This is the FNV-1a that
        // Core::ShaderVariant::Hash() computes over the same bytes — the number that becomes the SPIR-V
        // cache key and, for a medium, part of Graphic::CloudEnvironmentFingerprint. Pinning it would
        // make every legitimate emitter change a table edit; printing it puts the identity of every
        // shipped graph in the log of every platform's job, so "macOS and Windows agree" is one diff
        // away instead of a belief.
        uint64_t hash = 1469598103934665603ull;
        for ( const unsigned char c : first.GetValue() )
        {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        std::printf( "[ShaderGraphDeterminism] %-28s text fnv1a=%016llx\n",
                     entry.path().filename().string().c_str(), static_cast<unsigned long long>( hash ) );
        ++seen;
    }
    ASSERT_GT( seen, 0 ) << "no .dgraph was found at all, so this test measured nothing";
}
