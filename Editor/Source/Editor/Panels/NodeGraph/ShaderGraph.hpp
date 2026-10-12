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
    // Shader Language (.shader) source, and the reconstruction that rebuilds a graph's pins against the current
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
            // The palette submenu the node is listed under; nullptr = the palette's top level.
            const char* Category = nullptr;
        };

        // The palette category of the particle nodes (UE's "Particles" expression category).
        inline constexpr const char* kParticlesCategory = "Particles";

        /// True for the node kinds that read the particle half of SurfaceInput (i.Particle), which only the
        /// ParticleSprite cell fills: a graph using one must be UsedWithParticleSprites.
        [[nodiscard]] bool ReadsParticleInputs( const std::string& kind );

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

        // REBUILD EVERY NODE'S PINS FROM THE CURRENT CATALOGUE — run on every load, like UE's
        // UEdGraphSchema::ReconstructNode / UK2Node::ReconstructNode (EdGraphSchema.cpp, K2Node.cpp:703).
        // It is not a format migration: nothing is written back, and a document whose nodes already
        // match the catalogue comes out unchanged.
        //
        // Per node of a kind the catalogue knows, both pin lists are reallocated in catalogue order and
        // the stored pins are rewired onto them (UE's RewireOldPinsToNewPins):
        //   * a stored pin whose NAME and TYPE match a catalogue pin keeps its id, so every link to it
        //     still lands where it landed;
        //   * a catalogue pin with no stored match is new and takes its id from doc.NextId;
        //   * a stored pin with no match that a link still uses is kept AFTER the catalogue pins as an
        //     ORPHAN (UE's bOrphanedPin) — the link is never silently rebound to a different pin, and
        //     ValidateGraph refuses the node by the orphan's name;
        //   * a stored pin with no match and no link is dropped (UE discards unconnected old pins).
        // A node of an unknown kind is left as it is; ValidateGraph names it.
        //
        // Pure: takes a document, touches no file and no global state.
        void ReconstructNodes( Document& doc );

        // JSON -> document, THEN its nodes reconstructed against the current catalogue. The writing half
        // is not here: it is `Assets::Serialization::ShaderGraph::Serialize`, which needs no catalogue and
        // belongs with the bytes. This one does need it, which is exactly why the two halves sit on
        // opposite sides of the seam — see the note at the top of the engine header.
        Common::ResultStr<Document> Deserialize( const std::string& json );
    } // namespace ShaderGraph
} // namespace Desert::Editor
