#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Assets/Serialization/ShaderGraph.hpp>
#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    // The shader graph's SEMANTICS: the node catalogue, the compiler that turns a graph into Desert
    // Shader Language (.shader) source, and the migration that brings an older graph up to the current
    // catalogue. The panel owns the interactive canvas; the FILE — pins, nodes, links, the JSON round
    // trip — is Engine/Assets/Serialization/ShaderGraph.hpp, and the block below says why.
    namespace ShaderGraph
    {
        // ── THE FILE LIVES IN THE ENGINE; WHAT A NODE MEANS LIVES HERE ───────────────────────────
        //
        // `Pin`, `Node`, `Link` and `Document` moved to Engine/Assets/Serialization/ShaderGraph.hpp the
        // day a `.dgraph` became an asset: the handle-stability census, the content scan and the
        // eviction walk all have to name `ShaderGraphAsset`, and none of them can see `Editor/`. They
        // are ALIASED back rather than re-declared — one definition, two spellings — because the
        // catalogue and the compiler below are written in these names and the seam is what is new, not
        // the model.
        //
        // The DOMAIN and the VALUE TYPE travel with the document for the reason stated at their
        // definitions: both are stored as integers in the file, so they are format, not semantics.
        using ValueType = ::Desert::Assets::Serialization::ShaderGraph::ValueType;
        using Domain    = ::Desert::Assets::Serialization::ShaderGraph::Domain;
        using Pin       = ::Desert::Assets::Serialization::ShaderGraph::Pin;
        using Node      = ::Desert::Assets::Serialization::ShaderGraph::Node;
        using Link      = ::Desert::Assets::Serialization::ShaderGraph::Link;
        using Document  = ::Desert::Assets::Serialization::ShaderGraph::Document;

        // The extension and the content folder, read through the format's own constants so this side
        // never spells either a second time.
        inline constexpr std::string_view kExtension =
             ::Desert::Assets::Serialization::ShaderGraph::kShaderGraphExtension;

        // Where the graph's OWN textures start in the descriptor set: the first slot of the window
        // reserved for graph-owned resources in EVERY domain, which the engine owns and a census keeps
        // free (Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp). Not a second number here — the
        // runtime binds into the same window, and the two halves of one reservation cannot be two
        // constants.
        constexpr unsigned kGraphTextureBinding = ::Desert::Core::kGraphOwnedBindingFirst;

        // Bit for one domain; a NodeSpec lists the domains it belongs to as a mask.
        constexpr unsigned DomainBit( Domain d )
        {
            return 1u << static_cast<int>( d );
        }
        // "Core" nodes (math, Time, textures, params) live in every domain.
        constexpr unsigned AllDomains = ~0u;

        // Static description of a node kind — drives BOTH the palette/UI and the compiler.
        struct NodeSpec
        {
            const char* Kind;
            const char* Title;
            unsigned    HeaderColor; // IM_COL32 value
            struct PinSpec
            {
                const char* Name;
                ValueType   Type;
            };
            std::vector<PinSpec> Inputs;
            std::vector<PinSpec> Outputs;
            bool                 HasParamName = false; // shows a name field, emits a Property
            bool                 HasColorValue = false; // shows a vec4 editor
            bool                 HasFloatValue = false; // shows a float editor
            unsigned             Domains = AllDomains;  // which domains this node is offered in
        };

        const std::vector<NodeSpec>& Specs();
        const NodeSpec*              FindSpec( const std::string& kind );

        // ---- The Volume domain's material-parameter register -------------------------------------
        //
        // WHICH OF THE CLOUD MATERIAL'S OWN PROPERTIES A GRAPH NODE MAY READ, and it is a REGISTER with a
        // reason per row rather than a list, because the interesting half is what is NOT here.
        //
        // ABOUT HALF OF THAT MATERIAL'S VALUES ARE INPUTS TO A CPU BAKE — a 256x32x256 volume over
        // several thousand cloud bodies, 3.3 to 14.1 seconds — and the graph runs on the GPU, per sample,
        // inside a march that reads the RESULT of that bake. A `Timing(Rebake)` property is therefore not
        // merely inconvenient to reach from here: it is not in the shader's scope at all, and a node
        // pretending to read one would either fail to compile or, worse, read a same-named field that
        // means something else. The split is shown to the author in the Material Editor (every property
        // states its Timing) and is made UNEXPRESSIBLE here.
        //
        // AND IT IS A TEST, NOT A CONVENTION. Desert/Tests/Editor/ShaderGraphVolumeDomain parses the
        // shipped CloudRaymarch.shader and asserts, in both directions:
        //   * every row below names a property that exists and is Timing(Immediate) — so exposing a bake
        //     input goes RED;
        //   * every Immediate property of the schema is either a row below or a row of the out-of-scope
        //     register beside it, with its reason — so a NEW property cannot be added without somebody
        //     deciding whether the graph may read it.
        // There is no hand-written name list anywhere in that suite.
        struct VolumeParam
        {
            const char* SchemaName; // the property's name in CloudRaymarch.shader's Properties block
            const char* Expression; // the GLSL it becomes inside a Medium block
            const char* Units;      // what the number IS at that point, which is not always what the panel shows
        };
        const std::vector<VolumeParam>& VolumeParams();

        /// An Immediate property the graph deliberately does NOT expose, and why. See VolumeParams().
        struct VolumeParamOutOfScope
        {
            const char* SchemaName;
            const char* Reason;
        };
        const std::vector<VolumeParamOutOfScope>& VolumeParamsOutOfScope();

        /// A Volume Output pin in which the Cloud Sample node's `ShadowRay` output means something.
        ///
        /// The medium is five functions and only TWO of them are ever called by a march that integrates
        /// optical depth — the sun quadrature, the cloud shadow map and the sky-occlusion volume all ask
        /// for a density and an extinction and nothing else. In the other three `ShadowRay` is the
        /// constant zero, so a graph branching on it there would be authoring a path that can never be
        /// taken: a dead knob, which this project's contract refuses in the same breath as a stub.
        ///
        /// Desert/Tests/Editor/ShaderGraphCompiler DERIVES this set from the shader tree — it reads which
        /// entry points the three shadow-ray marches actually call — and compares it with the rows below,
        /// so the day a shadow march starts asking for an albedo this register goes red instead of the
        /// author's branch quietly disappearing.
        struct ShadowRayScope
        {
            const char* OutputPin;  // the Volume Output input the medium function is compiled from
            const char* EntryPoint; // the GLSL function a shadow-ray march calls
        };
        const std::vector<ShadowRayScope>& ShadowRayScopes();

        // Node kind that terminates a graph in the given domain (SurfaceOutput / PostProcessOutput).
        const char* OutputKind( Domain domain );

        // Is this node kind available in the given domain?
        bool SpecInDomain( const NodeSpec& spec, Domain domain );

        // Instantiate a node of the given kind (allocates pin ids from doc.NextId).
        Node MakeNode( Document& doc, const std::string& kind );

        // Compile the graph to DShader source. Errors (no Surface Output, cycles, bad param
        // names) come back as the error string.
        Common::ResultStr<std::string> CompileToDShader( const Document& doc );

        // Bring every node in @p doc up to the CURRENT catalogue by appending the pins its kind has
        // grown since the document was written, and return how many pins were appended.
        //
        // Pins are stored positionally in a .dgraph and links reference them by id, so a pin that is
        // APPENDED changes nothing that already exists: index 0..n-1 keep their meaning and every
        // saved link still lands where it landed. That is also the limit of what can be repaired
        // here — a node whose stored pins are not a PREFIX of the catalogue's (a renamed pin, a
        // changed type, a reorder) is left exactly as it is, so ValidateGraph rejects it by name
        // instead of this function quietly rewriting the artist's graph into something else.
        //
        // THAT SENTENCE WAS A PROMISE THIS PAIR DID NOT KEEP until O1-J, and it is recorded rather
        // than quietly corrected because the shape recurs: ValidateGraph compared pin TYPES and not
        // pin NAMES, so a rename that kept the type was left alone here and then accepted there —
        // the one half of "not a prefix" that reached the emitter, which uses the stored name as a
        // GLSL struct member. Both functions now give the same answer to a renamed pin, refuse.
        //
        // Pure: takes a document, returns a document, touches no file and no global state.
        int MigrateToCatalogue( Document& doc );

        // A document as it came off disk, plus what had to change to make it current. Deserialize
        // hands back both TOGETHER and not a bare Document, because a silent migration is the thing
        // the contract forbids: the caller cannot be given the new document without also being told
        // how much of it is new.
        struct Loaded
        {
            Document Doc;
            int      MigratedPins = 0;
        };

        // JSON -> document, THEN up to the current node catalogue. The writing half is not here: it is
        // `Assets::Serialization::ShaderGraph::Serialize`, which needs no catalogue and belongs with the
        // bytes. This one does need it, which is exactly why the two halves sit on opposite sides of the
        // seam — see the note at the top of the engine header.
        Common::ResultStr<Loaded> Deserialize( const std::string& json );
    } // namespace ShaderGraph
} // namespace Desert::Editor
