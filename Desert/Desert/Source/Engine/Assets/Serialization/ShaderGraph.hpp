#pragma once

/**
 * @file ShaderGraph.hpp
 * @brief The `.dgraph` FILE, and nothing about what a node MEANS.
 *
 * WHY THE FORMAT MOVED OUT OF THE EDITOR. A `.dgraph` is now an asset — `AssetTypeID::ShaderGraph`,
 * `ShaderGraphAsset`, a handle a document is keyed on — and the asset system is the engine's. The
 * handle-stability census, the preloader's content scan and the eviction walk all have to name the
 * class, and none of them can see `Editor/`. So the DOCUMENT (pins, nodes, links, the JSON round trip)
 * lives here, beside `Serialization/ControlRig.hpp`, in exactly the same shape.
 *
 * WHAT DELIBERATELY DID NOT MOVE: the node CATALOGUE (`Editor::ShaderGraph::Specs()`), the compiler to
 * Desert Shader Language, and the node reconstruction against the catalogue. Those are authoring semantics — they
 * answer "what does a `Multiply` node do", which is a question no runtime asks, because the runtime reads the
 * COMPILED `.shader` and never a graph. Splitting them the other way would drag the whole node table and
 * the GLSL emitter into the engine for one file read.
 *
 * SO `Deserialize` IS NOT HERE EITHER, and that is the same line drawn once more: parsing bytes into a
 * `Document` is a format question and lives here; bringing a `Document` up to the CURRENT node catalogue
 * is an authoring question and stays with the catalogue. The editor's `Deserialize` is this file's
 * `ParseShaderGraph` followed by that reconstruction, and it is the only spelling either half has.
 */

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets::Serialization::ShaderGraph
{
    /// The extension the Content Browser, the path opener, the preloader and the loader all agree on.
    /// One constant, because a second spelling of it is a double-click that silently does nothing.
    inline constexpr std::string_view kShaderGraphExtension = ".dgraph";

    /// Where graphs live, RELATIVE to the project's assets root — the spelling `Constants::ContentDir`
    /// derives `Path::SHADER_GRAPH_PATH` from, asserted equal to it by the PathCensus suite rather than
    /// written twice.
    inline constexpr std::string_view kShaderGraphAssetsRelativeDir = "ShaderGraphs/";

    enum class ValueType : int
    {
        Float = 0,
        Vec2  = 1,
        Color = 2, // vec4
        // APPENDED AND NEVER REORDERED, like the pin lists below: a .dgraph stores Pin::Type as this
        // integer, so inserting a value would silently retype every saved pin above it. Vec3 exists
        // because the Volume domain's contract is written in three-component quantities that are NOT
        // colours-with-alpha — a position in kilometres, an albedo, an emission per kilometre — and
        // spelling them vec4 would make "what does .w mean here" a question with no answer.
        Vec3 = 3,
    };

    // Where a graph runs (mirrors UE's Material Domain / Godot's shader Mode). The domain is the
    // single axis that picks the output node, the vertex contract and the visible palette — the
    // whole graph is parameterized by it. Stored as int on the Document for reflection-friendly
    // serialization (same reason Pin::Type is an int).
    enum class Domain : int
    {
        Surface     = 0, // lit/unlit material on scene meshes (mesh vertex + normals)
        PostProcess = 1, // full-screen effect over the rendered scene color (fullscreen triangle)
        // THE CLOUD MEDIUM — what a cloud IS at a point in space, and the one domain that compiles to
        // a program FRAGMENT rather than to a program. Its output is a `Medium { ... }` block that
        // four shipped programs are compiled against (Docs/Clouds/O1_DESIGN.md §10.3); it has no
        // vertex contract, no framebuffer and no draw of its own, because it never draws — it is
        // substituted into things that do.
        Volume = 2,
        // THE WORLD'S SURFACE LAYER — what lies ON every lit surface of the world (snow, dirt, ash, moss), and
        // like Volume a FRAGMENT rather than a program: the graph compiles to one GLSL function
        // (Generated/SurfaceLayers.glslh) that every surface pass header and the landscape call where the
        // weather is applied, so the layer covers every material without one of them being edited (UE's Material
        // Layers are a stack inside EACH master material; here the world owns one layer). Which layer a world
        // shows is a world parameter (WorldParameterType::SurfaceLayer) naming the graph, so switching it is a
        // value, not a recompile; editing the graph regenerates the function and the surface programs reload.
        SurfaceLayer = 3,
    };

    /// A surface layer's id as the world parameter carries it: the low 24 bits of its graph name's FNV-1a, so the
    /// float the table stores holds it exactly. The dispatcher in Generated/SurfaceLayers.glslh compares the same.
    [[nodiscard]] constexpr uint32_t SurfaceLayerId( const std::string_view graphName )
    {
        uint32_t hash = 2166136261u;
        for ( const char c : graphName )
        {
            hash ^= static_cast<uint8_t>( c );
            hash *= 16777619u;
        }
        return ( hash & 0x00FFFFFFu ) | 0x00000001u; // never 0: 0 is "no layer"
    }

    /// The world parameter name a surface layer's texture is published under (and its key, hashed the same way as
    /// every world parameter): the layer's textures ride the world's bindless parameter table, no binding of their
    /// own.
    [[nodiscard]] inline std::string SurfaceLayerTextureName( const std::string_view textureAsset )
    {
        return std::string( "SurfaceLayer:" ) + std::string( textureAsset );
    }

    struct Pin
    {
        uint64_t    Id = 0;
        std::string Name;
        int         Type = 0; // ValueType (int for reflection-friendly serialization)
    };

    // Node semantics are identified by Kind. THE CATALOGUE IS Editor::ShaderGraph::Specs() AND IS NOT
    // REPEATED HERE — see the file note: this struct is what the bytes are, not what they mean.
    struct Node
    {
        uint64_t             Id = 0;
        std::string          Kind;
        std::string          ParamName;              // TextureSample / *Param nodes: exposed property name
        std::array<float, 4> Value = { 1, 1, 1, 1 }; // *Const / *Param nodes: (default) value
        std::string          Asset; // Layer Texture nodes: the texture asset (content-relative .detex), as UE's
                                    // Texture Sample holds its texture

        float                X = 0.0f, Y = 0.0f;     // canvas position (captured on save)
        std::vector<Pin>     Inputs;
        std::vector<Pin>     Outputs;
    };

    struct Link
    {
        uint64_t Id   = 0;
        uint64_t From = 0; // output pin id
        uint64_t To   = 0; // input pin id
    };

    /**
     * @brief One `.dgraph`, as written.
     *
     * NO GUID FIELD, AND THAT IS A MEASUREMENT RATHER THAN AN OMISSION. The note that stood at
     * FileExplorerPanel's hand-written arm listed "a `.dgraph` has no identity of its own" as the second
     * of four obstacles to making this window a document, and concluded that the format had to grow a
     * field and the graphs in the tree had to be migrated. It does not: `Common::AssetHandle` is derived
     * from the asset's PROJECT-RELATIVE PATH behind a root tag (`AssetHandle::FromKey` /
     * `StableKeyForPath`), deterministically, without reading a byte of the payload — which is how
     * `.derig`, `.detheme` and `.dclayout` all have stable handles while carrying no id of their own.
     * The obstacle was real for a format that stamps its identity into the file; this engine does not
     * have one of those. Zero `.dgraph` files changed when the type was registered.
     */
    struct Document
    {
        std::string       Name   = "GraphShader";
        uint64_t          NextId = 1;
        int               Domain = static_cast<int>( ShaderGraph::Domain::Surface ); // ShaderGraph::Domain
        bool              Lit    = false; // Surface-only: Lambert from the scene's directional light
        // Surface-only: UE's "Used with Particle Sprites" usage. The generated template declares
        // `Usage ParticleSprites`, which adds the ParticleSprite.Forward cell the particle pass draws with — and
        // it is the cell whose SurfaceInput carries the particle values the Particle nodes read, so those nodes
        // are refused by name in a graph that does not tick it.
        bool              UsedWithParticleSprites = false;
        std::vector<Node> Nodes;
        std::vector<Link> Links;

        [[nodiscard]] ShaderGraph::Domain DomainEnum() const
        {
            return static_cast<ShaderGraph::Domain>( Domain );
        }
    };

    /// The document as JSON. Never fails: every field has a value and reflect-cpp writes all of them.
    [[nodiscard]] std::string Serialize( const Document& doc );

    /// JSON -> document, with NO node reconstruction — see the file note for why that half is the
    /// editor's. Missing fields take their defaults (a graph written by an older build is readable);
    /// malformed JSON is an error carrying reflect-cpp's own message, never a quietly empty graph.
    [[nodiscard]] Common::ResultStr<Document> ParseShaderGraph( const std::string& json );

    /// The texture assets a Surface Layer graph samples (its Layer Texture nodes), in document order, unique.
    [[nodiscard]] std::vector<std::string> SurfaceLayerTextures( const Document& doc );

    // NO WRITE FUNCTION HERE, and it is a line worth keeping: this unit turns bytes into a document and
    // back, and nothing in it touches the disk. `ShaderGraphAsset::Save` is what writes, because the
    // moment a file is opened this translation unit needs Common's filesystem layer — which on macOS
    // drags the Objective-C file dialog into every suite that compiles it, and the compiler suite was
    // written to need Common's HEADERS and link nothing.
} // namespace Desert::Assets::Serialization::ShaderGraph
