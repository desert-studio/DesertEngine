#include "ShaderGraph.hpp"

#include <Engine/Core/ShaderCompiler/ShaderGraphMedium.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Editor::ShaderGraph
{
    // Header colour packed exactly like Dear ImGui's IM_COL32 (R at bit 0). Kept local so this file —
    // the graph SEMANTICS and compiler — stays free of any ImGui dependency and is unit-testable on
    // its own; the panel reads NodeSpec::HeaderColor back as an ImU32.
    static constexpr unsigned RGBA( unsigned r, unsigned g, unsigned b, unsigned a )
    {
        return ( a << 24 ) | ( b << 16 ) | ( g << 8 ) | r;
    }

    // ---------------------------------------------------------------- node catalogue ----------
    // Domain masks for the two families of nodes:
    //   CORE    — math / textures / params / Time: valid in every domain.
    //   SURFACE / POST — output nodes and domain-specific inputs, offered only in their own domain.
    static constexpr unsigned CORE    = AllDomains;
    static constexpr unsigned SURFACE = DomainBit( Domain::Surface );
    static constexpr unsigned POST    = DomainBit( Domain::PostProcess );
    static constexpr unsigned VOLUME  = DomainBit( Domain::Volume );

    // Nodes that are valid everywhere EXCEPT the cloud medium. What a volume has none of is UVs and scene
    // colour — there is a `v_UV` in neither a compute march nor a participating medium, and no rendered
    // scene colour to read back.
    //
    // ITS THIRD MEMBER USED TO BE "no exposed properties or textures of its own", AND THAT IS GONE. Two
    // successive reasons were given for it and both were retired by measurement: first "a collision
    // between two GLSL declarations at one binding is silent", which Г17 had already made false (the
    // engine refuses it by name at reflection, in all four consumers); then "a cloud material has no place
    // to keep a value whose name comes from a graph", which is what О1-G-2 built — the medium carries its
    // own Properties block, its values are filed in the `.demat` under Core::kCloudMediumOverridePrefix so
    // they can never be read as the shipped schema's, and they reach all four consumers through one
    // storage buffer and up to Core::kCloudMediumMaxTextures samplers in the reserved window.
    //
    // TextureSample is still not here, and that is a scope fact rather than a leftover: it samples at
    // `v_UV`. A medium samples at a place the march hands it, so it has a node of its own (MediumTexture)
    // whose coordinates are two floats the graph computes.
    static constexpr unsigned NOT_VOLUME = CORE & ~VOLUME;

    // The Category every medium property is emitted under, so the material window shows them as one group
    // under a heading that says where they came from rather than mixed into the shipped schema's rows.
    static constexpr const char* kMediumCategory = "Medium";

    // The catalogue is a TABLE and is kept as one: one node per visual row, pins grouped on their own
    // line. Left to itself clang-format explodes every entry into eleven lines of one field each,
    // because the braced initializer no longer fits a line once a node has six pins — 250 lines of
    // vertical noise for a list whose whole value is being scannable side by side. Same reason
    // PipelineCache.hpp and TProperty.hpp fence their tables.
    // clang-format off
    const std::vector<NodeSpec>& Specs()
    {
        static const std::vector<NodeSpec> s_Specs = {
            // ---- domain-specific: outputs & special inputs ----
            // The MATERIAL ATTRIBUTES a lit surface hands the engine's shading model, in the order
            // they were added: Albedo/Emission/Alpha first, then the three the shared PBR texts need
            // (Mesh/AmbientIBL.glslh and Mesh/DirectLighting.glslh both take metalness and roughness,
            // and the ambient takes an occlusion factor). APPENDED and never reordered — a saved
            // .dgraph stores pins positionally, and MigrateToCatalogue below can only grow a node
            // whose stored pins are still a prefix of this list.
            { "SurfaceOutput", "Surface Output", RGBA( 150, 90, 60, 255 ),
              { { "Albedo", ValueType::Color },
                { "Emission", ValueType::Color },
                { "Alpha", ValueType::Float },
                { "Metallic", ValueType::Float },
                { "Roughness", ValueType::Float },
                { "Occlusion", ValueType::Float } },
              {}, false, false, false, SURFACE },
            { "PostProcessOutput", "Post Process Output", RGBA( 150, 90, 60, 255 ),
              { { "Color", ValueType::Color } }, {}, false, false, false, POST },
            { "SceneColor", "Scene Color", RGBA( 70, 110, 160, 255 ), {},
              { { "Color", ValueType::Color } }, false, false, false, POST },
            // THE CLOUD MEDIUM'S FIVE OUTPUTS — the Volume domain's contract (O1_DESIGN §3.3, accepted
            // §9 п.2). An UNCONNECTED pin is not zero: it is the SHIPPED default for that output, which
            // is what makes a half-authored graph a modification of the sky rather than a deletion of it.
            { "VolumeOutput", "Volume Output", RGBA( 150, 90, 60, 255 ),
              { { "Density", ValueType::Float },
                { "Extinction", ValueType::Float },
                { "Albedo", ValueType::Vec3 },
                { "Emissive", ValueType::Vec3 },
                { "AmbientOcclusion", ValueType::Float } },
              {}, false, false, false, VOLUME },
            // WHAT THE MARCH KNOWS AT THIS POINT IN THE SKY. Everything the four consumers hand the
            // medium, and nothing else: there is no UV, no normal and no mesh in a volume.
            { "CloudSample", "Cloud Sample", RGBA( 70, 110, 160, 255 ), {},
              { { "PositionKm", ValueType::Vec3 },
                { "WindPositionKm", ValueType::Vec3 },
                { "Profile", ValueType::Float },
                { "DetailType", ValueType::Float },
                { "DetailFactor", ValueType::Float },
                { "DensityScale", ValueType::Float },
                { "ExtinctionFactor", ValueType::Float },
                // WHICH MARCH IS ASKING — 0 for the eye, 1 for a quadrature that only wants a
                // transmittance. APPENDED, because a saved .dgraph stores pins positionally. It is legal
                // in the two outputs a shadow march actually calls and refused in the other three, where
                // it could only ever be the constant zero; ShadowRayScopes() below is that register.
                { "ShadowRay", ValueType::Float },
                // WHICH `.dcnv` THE WINNING SPECIES' EDGE IS CUT FROM. APPENDED for the same reason
                // ShadowRay was. It is what makes the population of sculpted noise volumes a project owns
                // reachable from a canvas at all, and it ships in the same change as the node that
                // consumes it — on its own it would be an index an artist can wire nowhere.
                { "NoiseSlot", ValueType::Float } },
              false, false, false, VOLUME },
            // THE LAYER'S OWN NOISE VOLUME, sampled where and at whichever slot the graph says.
            //
            // NOT THE MEDIUM TEXTURE NODE WITH A THIRD COORDINATE. That one samples an image the MATERIAL
            // carries and pays a descriptor for in all four consumers; this one samples a `.dcnv` the SCENE
            // has already bound for the species standing at this sample, so it costs no binding, no
            // property and no runtime plumbing at all. Without it a graph writing its own density may
            // inherit the shipped erosion whole or invent a field from nothing and nothing in between —
            // and "the same volume, eroded differently" is exactly the in-between.
            //
            // BOTH INPUTS UNWIRED IS THE SHIPPED FETCH, the same convention an unwired Volume Output pin
            // follows: the slot is the winner's own and the coordinate is the one the shipped chain reads
            // at. Four Float outputs and no vec4 one, because this domain has no vec4 sink anywhere, so an
            // RGBA pin here could be wired nowhere — the dead-knob refusal Vec3Param was introduced for.
            { "CloudNoise", "Cloud Noise Volume", RGBA( 70, 110, 160, 255 ),
              { { "Slot", ValueType::Float }, { "Coordinate", ValueType::Vec3 } },
              { { "WispyCoarse", ValueType::Float },
                { "WispyFine", ValueType::Float },
                { "BillowyCoarse", ValueType::Float },
                { "BillowyFine", ValueType::Float } },
              false, false, false, VOLUME },
            // The layer's own two lighting values, each legal ONLY in the output it belongs to — see the
            // reachability rule in the compiler. They exist so a graph can MODIFY what the layer decided
            // (tint the material's albedo, multiply the occlusion volume's answer) instead of being
            // forced to replace a pass it cannot see.
            // THE SHIPPED MEDIUM, callable. An authored cloud is almost always a MODIFICATION of the
            // engine's — "the same clouds, thinner over there" — and a graph that had to rebuild the
            // erosion chain by hand to say that would be a graph nobody could use. Their bodies live in
            // Common/CloudMediumDefault.glslh, which a material never substitutes, so they stay reachable
            // from an authored medium.
            { "DefaultDensity", "Default Density", RGBA( 70, 110, 160, 255 ), {},
              { { "Density", ValueType::Float } }, false, false, false, VOLUME },
            { "DefaultExtinctionFactor", "Default Extinction Factor", RGBA( 70, 110, 160, 255 ), {},
              { { "Factor", ValueType::Float } }, false, false, false, VOLUME },
            { "LayerAlbedo", "Layer Albedo", RGBA( 160, 80, 90, 255 ), {},
              { { "Albedo", ValueType::Vec3 } }, false, false, false, VOLUME },
            { "LayerOcclusion", "Layer Occlusion", RGBA( 90, 140, 90, 255 ), {},
              { { "Occlusion", ValueType::Float } }, false, false, false, VOLUME },
            // A read of one of the CLOUD MATERIAL's own properties. Which ones are readable is the
            // register in VolumeParams(); a bake input is not among them and cannot be made one here.
            { "CloudParam", "Cloud Material Param", RGBA( 160, 80, 90, 255 ), {},
              { { "Value", ValueType::Float } }, /*param*/ true, false, false, VOLUME },
            { "Vec3Const", "Vector 3", RGBA( 120, 70, 80, 255 ), {},
              { { "Vector", ValueType::Vec3 } }, false, /*color*/ true, false, VOLUME },
            { "MultiplyVec3", "Multiply (Vector 3)", RGBA( 90, 90, 120, 255 ),
              { { "A", ValueType::Vec3 }, { "B", ValueType::Vec3 } },
              { { "Out", ValueType::Vec3 } }, false, false, false, VOLUME },
            { "ScaleVec3", "Scale (Vector 3 x Float)", RGBA( 90, 90, 120, 255 ),
              { { "Vector", ValueType::Vec3 }, { "Factor", ValueType::Float } },
              { { "Out", ValueType::Vec3 } }, false, false, false, VOLUME },
            { "SplitVec3", "Split (Vector 3)", RGBA( 110, 110, 110, 255 ),
              { { "In", ValueType::Vec3 } },
              { { "X", ValueType::Float }, { "Y", ValueType::Float }, { "Z", ValueType::Float } },
              false, false, false, VOLUME },
            // ---- core: valid everywhere ----
            { "TextureSample", "Texture Sample", RGBA( 70, 110, 160, 255 ),
              { { "UV", ValueType::Vec2 } },
              { { "RGBA", ValueType::Color }, { "R", ValueType::Float } },
              /*param*/ true, false, false, NOT_VOLUME },
            { "ColorParam", "Color Param", RGBA( 160, 80, 90, 255 ), {},
              { { "Color", ValueType::Color } }, /*param*/ true, /*color*/ true, false, NOT_VOLUME },
            { "FloatParam", "Float Param", RGBA( 90, 140, 90, 255 ), {},
              { { "Value", ValueType::Float } }, /*param*/ true, false, /*float*/ true, CORE },
            // A Vec3 AND NOT A COLOR PARAM, because a vec4 has nowhere to go in this domain: the medium's
            // three-component outputs are an albedo and an emission per kilometre, and the only vec4 sink
            // in the palette is none at all. A Color Param offered here would be a node that can never be
            // wired to anything — a dead knob, refused in the same breath as a stub. It is emitted as the
            // DSL's `Color3`, so the material window still shows it as a colour swatch.
            { "Vec3Param", "Vector 3 Param", RGBA( 160, 80, 90, 255 ), {},
              { { "Vector", ValueType::Vec3 } }, /*param*/ true, /*color*/ true, false, VOLUME },
            // THE MEDIUM'S OWN IMAGE, sampled where the graph says rather than at a `v_UV` a volume does
            // not have. Two floats and not a Vec2 because the Volume palette's arithmetic is float and
            // vec3: a coordinate is almost always built out of Split (Vector 3) of the sample position,
            // and a Vec2 pin would need a make-vec2 node whose only purpose is to feed this one.
            //
            // RGB AND NOT RGBA, for the same reason Vec3Param exists instead of a Color Param here: this
            // domain has no vec4 sink anywhere in its palette, so a Color pin would be a handle the canvas
            // refuses every link out of — a dead knob, which the contract forbids in the same breath as a
            // stub. Retyped rather than removed: a .dgraph stores pins POSITIONALLY, so dropping output 0
            // would move the R pin under every saved link. As Vec3 the pin is live — Albedo, Emissive and
            // Multiply (Vector 3) all take it. `NoPinIsOfferedInADomainThatCannotConnectIt` is the gate.
            { "MediumTexture", "Medium Texture", RGBA( 70, 110, 160, 255 ),
              { { "U", ValueType::Float }, { "V", ValueType::Float } },
              { { "RGB", ValueType::Vec3 }, { "R", ValueType::Float } },
              /*param*/ true, false, false, VOLUME },
            { "ColorConst", "Color", RGBA( 120, 70, 80, 255 ), {},
              { { "Color", ValueType::Color } }, false, /*color*/ true, false, NOT_VOLUME },
            { "FloatConst", "Float", RGBA( 70, 110, 70, 255 ), {},
              { { "Value", ValueType::Float } }, false, false, /*float*/ true, CORE },
            { "UV", "UV", RGBA( 150, 130, 60, 255 ), {}, { { "UV", ValueType::Vec2 } },
              false, false, false, NOT_VOLUME },
            { "TileUV", "Tile UV", RGBA( 150, 130, 60, 255 ),
              { { "UV", ValueType::Vec2 }, { "Scale", ValueType::Float } },
              { { "UV", ValueType::Vec2 } }, false, false, false, /*mesh tiling*/ SURFACE },
            { "Multiply", "Multiply", RGBA( 90, 90, 120, 255 ),
              { { "A", ValueType::Color }, { "B", ValueType::Color } },
              { { "Out", ValueType::Color } }, false, false, false, NOT_VOLUME },
            { "Scale", "Scale (Color x Float)", RGBA( 90, 90, 120, 255 ),
              { { "Color", ValueType::Color }, { "Factor", ValueType::Float } },
              { { "Out", ValueType::Color } }, false, false, false, NOT_VOLUME },
            { "Add", "Add", RGBA( 90, 90, 120, 255 ),
              { { "A", ValueType::Color }, { "B", ValueType::Color } },
              { { "Out", ValueType::Color } }, false, false, false, NOT_VOLUME },
            { "Lerp", "Lerp", RGBA( 120, 90, 130, 255 ),
              { { "A", ValueType::Color }, { "B", ValueType::Color }, { "T", ValueType::Float } },
              { { "Out", ValueType::Color } }, false, false, false, NOT_VOLUME },
            { "OneMinus", "One Minus", RGBA( 110, 110, 110, 255 ),
              { { "In", ValueType::Color } }, { { "Out", ValueType::Color } }, false, false, false, NOT_VOLUME },
            { "MultiplyFloat", "Multiply (Float)", RGBA( 80, 120, 80, 255 ),
              { { "A", ValueType::Float }, { "B", ValueType::Float } },
              { { "Out", ValueType::Float } }, false, false, false, CORE },
            { "AddFloat", "Add (Float)", RGBA( 80, 120, 80, 255 ),
              { { "A", ValueType::Float }, { "B", ValueType::Float } },
              { { "Out", ValueType::Float } }, false, false, false, CORE },
            { "SaturateFloat", "Saturate (Float)", RGBA( 110, 110, 110, 255 ),
              { { "In", ValueType::Float } }, { { "Out", ValueType::Float } }, false, false, false, CORE },
            { "PowerFloat", "Power (Float)", RGBA( 90, 90, 120, 255 ),
              { { "In", ValueType::Float }, { "Exp", ValueType::Float } },
              { { "Out", ValueType::Float } }, false, false, false, CORE },
            { "LerpFloat", "Lerp (Float)", RGBA( 120, 90, 130, 255 ),
              { { "A", ValueType::Float }, { "B", ValueType::Float }, { "T", ValueType::Float } },
              { { "Out", ValueType::Float } }, false, false, false, CORE },
            { "Saturate", "Saturate", RGBA( 110, 110, 110, 255 ),
              { { "In", ValueType::Color } }, { { "Out", ValueType::Color } }, false, false, false, NOT_VOLUME },
            { "Power", "Power", RGBA( 90, 90, 120, 255 ),
              { { "In", ValueType::Color }, { "Exp", ValueType::Float } },
              { { "Out", ValueType::Color } }, false, false, false, NOT_VOLUME },
            { "Sine", "Sine (Float)", RGBA( 80, 120, 80, 255 ),
              { { "In", ValueType::Float } }, { { "Out", ValueType::Float } }, false, false, false, CORE },
            // NOT IN THE VOLUME DOMAIN. `timeUB` is a uniform block the cloud programs do not declare —
            // and a medium that scrolled with wall-clock time would fight the layer's own wind offset,
            // which is the frame the modelling volume was BAKED in. Wind belongs to the sample position
            // the Cloud Sample node already hands out.
            { "Time", "Time", RGBA( 60, 140, 150, 255 ), {},
              { { "Seconds", ValueType::Float } }, false, false, false, NOT_VOLUME },
        };
        return s_Specs;
    }
    // clang-format on

    const NodeSpec* FindSpec( const std::string& kind )
    {
        for ( const auto& spec : Specs() )
            if ( kind == spec.Kind )
                return &spec;
        return nullptr;
    }

    // clang-format off
    const std::vector<VolumeParam>& VolumeParams()
    {
        // THREE, AND THE NUMBER IS A FACT ABOUT SCOPE RATHER THAN A CHOICE. A Medium block is compiled
        // into Common/CloudField.glslh, which every consumer includes BEFORE Common/CloudParams.glslh —
        // so the packed `u_Cloud*` block is not declared yet at that point, and the only material values
        // in scope are the ones the producer seam already carries in `CloudFieldParams`. That is a
        // property of the include order of four shipped programs, not of this table: moving the include
        // would widen it, and O1_DESIGN §10.3 does not ask for that.
        static const std::vector<VolumeParam> s_Params = {
            { "DetailTileSize", "params.DetailTileKm",
              "KILOMETRES. The Material Editor shows this property in centimetres, because that is the "
              "world unit; the medium's own maths is kilometre-scaled and this is the converted value." },
            { "DetailStrength", "params.DetailStrength", "0..1, the LAYER's erosion depth" },
            { "DensityScale",   "params.DensityScale",   "the LAYER's density multiplier" },
        };
        return s_Params;
    }

    const std::vector<VolumeParamOutOfScope>& VolumeParamsOutOfScope()
    {
        // EVERY Immediate PROPERTY THAT IS NOT READABLE, WITH ITS REASON. The suite derives the schema's
        // Immediate set and demands that each member be in one register or the other, so a property added
        // to CloudRaymarch.shader cannot slip past without somebody answering this question. Eleven rows,
        // and ten of them share one reason — stated per row anyway, because a shared reason written once
        // above the block is a reason that stops being checked when a row is added under it.
        static const std::vector<VolumeParamOutOfScope> s_OutOfScope = {
            { "ExtinctionScale",          "in the packed block, which is not declared where a Medium block is compiled; it reaches the medium as the Extinction OUTPUT's own multiplicand instead" },
            { "ScatteringAlbedo",         "reaches the graph as the Layer Albedo node, which is the same value at the one output where it is in scope" },
            { "PhaseG",                   "in the packed block, and a per-sample phase is not an output of this domain's contract" },
            { "PhaseGBackward",           "in the packed block, and a per-sample phase is not an output of this domain's contract" },
            { "PhaseBlend",               "in the packed block, and a per-sample phase is not an output of this domain's contract" },
            { "AmbientOcclusionStrength", "already applied to the value the Layer Occlusion node hands out, so exposing it as well would let a graph apply it twice" },
            { "MultiScatterOctaves",      "in the packed block, and the scattering series is read once per dispatch rather than per sample" },
            { "MultiScatterContribution", "in the packed block, and the scattering series is read once per dispatch rather than per sample" },
            { "MultiScatterOcclusion",    "in the packed block, and the scattering series is read once per dispatch rather than per sample" },
            { "MultiScatterEccentricity", "in the packed block, and the scattering series is read once per dispatch rather than per sample" },
            { "AmbientScale",             "in the packed block; it tints the sky term the march adds AFTER the medium has answered" },
        };
        return s_OutOfScope;
    }

    const std::vector<ShadowRayScope>& ShadowRayScopes()
    {
        // TWO, AND THE NUMBER IS A FACT ABOUT THE MARCHES rather than a policy. The three quadratures
        // that integrate optical depth — CloudLightOpticalDepth in Common/CloudField.glslh, the cloud
        // shadow map and the sky-occlusion volume — call exactly these two entry points and no others;
        // an albedo, an emission and an occlusion are only ever asked for by a march that is producing
        // radiance, where ShadowRay is the literal zero. The suite reads those three files and derives
        // this set, so the row and the shader cannot drift apart in silence.
        static const std::vector<ShadowRayScope> s_Scopes = {
            { "Density",    "CloudSampleDensity" },
            { "Extinction", "CloudSampleExtinctionFactor" },
        };
        return s_Scopes;
    }
    // clang-format on

    // Is the Volume Output pin currently being compiled one a shadow-ray march actually reaches?
    // Empty means "not inside a Volume medium at all", which is every other domain and is never in scope.
    static bool ShadowRayIsInScope( const std::string& outputPin )
    {
        for ( const auto& scope : ShadowRayScopes() )
            if ( outputPin == scope.OutputPin )
                return true;
        return false;
    }

    // The scope register as prose, for the refusal message. Built from the register rather than typed
    // beside it, so a row added there appears in what the artist is told without anybody remembering to.
    static std::string ShadowRayScopeList()
    {
        std::string list;
        for ( const auto& scope : ShadowRayScopes() )
        {
            if ( !list.empty() )
                list += ", ";
            list += scope.OutputPin;
        }
        return list;
    }

    const char* OutputKind( Domain domain )
    {
        switch ( domain )
        {
            case Domain::PostProcess:
                return "PostProcessOutput";
            case Domain::Volume:
                return "VolumeOutput";
            case Domain::Surface:
                break;
        }
        return "SurfaceOutput";
    }

    bool SpecInDomain( const NodeSpec& spec, Domain domain )
    {
        return ( spec.Domains & DomainBit( domain ) ) != 0;
    }

    Node MakeNode( Document& doc, const std::string& kind )
    {
        const NodeSpec* spec = FindSpec( kind );
        Node            node;
        node.Id   = doc.NextId++;
        node.Kind = kind;
        if ( spec )
        {
            for ( const auto& pin : spec->Inputs )
                node.Inputs.push_back( { doc.NextId++, pin.Name, static_cast<int>( pin.Type ) } );
            for ( const auto& pin : spec->Outputs )
                node.Outputs.push_back( { doc.NextId++, pin.Name, static_cast<int>( pin.Type ) } );
            if ( spec->HasParamName )
            {
                // A Cloud Material Param names a property of an EXISTING schema, so its default has to be
                // one that exists — "Amount" would make every freshly dropped node an error the artist
                // has to fix before the graph will compile at all.
                node.ParamName =
                     kind == "CloudParam"
                          ? VolumeParams().front().SchemaName
                          : ( spec->HasColorValue ? "Tint" : ( spec->HasFloatValue ? "Amount" : "u_Texture" ) );
            }
            if ( spec->HasFloatValue )
                node.Value = { 1, 0, 0, 0 };
        }
        return node;
    }

    // ---------------------------------------------------------------- compiler ----------------
    namespace
    {
        // Defined below with the validator, declared here because the compiler's own refusals name nodes
        // the same way the validator's do — one phrasing for both, so an artist reads the same sentence
        // whichever half rejected the graph.
        std::string NodeLabel( const Node& node );

        struct Compiler
        {
            const Document&                                doc;
            std::unordered_map<uint64_t, const Node*>      nodeByPin;   // any pin id -> node
            std::unordered_map<uint64_t, uint64_t>         linkIntoPin; // input pin id -> source OUTPUT pin id
            std::unordered_map<const Node*, std::string>   varOf;       // node -> emitted variable
            std::unordered_map<const Node*, int>           state;       // 0=unvisited 1=visiting 2=done
            std::ostringstream                             body;
            std::string                                    error;
            int                                            nextVar = 0;

            // WHICH OF THE MEDIUM'S FIVE OUTPUTS IS BEING EMITTED, or empty outside the Volume domain.
            //
            // It exists because two of the Volume nodes are only IN SCOPE in one output each: the medium
            // is compiled as five separate functions, and `materialAlbedo` is an argument of exactly one
            // of them. A node placed in the wrong one would emit GLSL that names an undeclared variable —
            // a compile error from generated code, naming a line the artist never wrote, which is the
            // failure ValidateGraph was written to stop. Here it is a refusal that names the node and the
            // pin instead.
            std::string currentOutput;

            // EVERY PROPERTY NODE THAT ACTUALLY REACHED THE OUTPUT, and it is collected rather than
            // assumed for a reason with two halves.
            //
            // The contract half: a property no function reads is a dead setting — a row in the material
            // window that moves nothing. Emitting only what is reached makes that unexpressible instead of
            // reviewable.
            //
            // The mechanical half, which is the one that would have cost a day: a Volume medium's
            // declarations are BOUND from C++ by number, and this backend writes a descriptor for every
            // binding the caller names. A block the emitter declared and no function read is eliminated
            // from the SPIR-V, so the write would land on a binding the layout does not have — a
            // validation error at best. Declared == statically referenced, by construction.
            std::unordered_set<const Node*> touchedParams;

            explicit Compiler( const Document& d ) : doc( d )
            {
                for ( const auto& node : doc.Nodes )
                {
                    for ( const auto& pin : node.Inputs )
                        nodeByPin[pin.Id] = &node;
                    for ( const auto& pin : node.Outputs )
                        nodeByPin[pin.Id] = &node;
                }
                for ( const auto& link : doc.Links )
                    linkIntoPin[link.To] = link.From;
            }

            static std::string Lit( float v )
            {
                return std::format( "{:.6g}", v ).find( '.' ) == std::string::npos &&
                               std::format( "{:.6g}", v ).find( 'e' ) == std::string::npos
                            ? std::format( "{:.1f}", v )
                            : std::format( "{:.6g}", v );
            }

            static std::string Vec4Lit( const std::array<float, 4>& v )
            {
                return std::format( "vec4( {}, {}, {}, {} )", Lit( v[0] ), Lit( v[1] ), Lit( v[2] ),
                                    Lit( v[3] ) );
            }

            /// Does the artist's canvas actually feed this input?
            ///
            /// Asked because for one node the FALLBACK is not a constant but a call, and the two have to
            /// be told apart before either is written: the Cloud Noise Volume node's unwired slot is
            /// `field.NoiseSlot`, an int straight off the producer, while a wired one is a float that has
            /// to be rounded and clamped back into an index. Deriving that from the returned string would
            /// be a comparison against text this function is free to change.
            bool IsLinked( const Node& node, size_t inputIndex ) const
            {
                return linkIntoPin.count( node.Inputs[inputIndex].Id ) != 0;
            }

            // Expression feeding @p inputPin of @p node, or the type's default when unlinked.
            //
            // NAMED `Emit...` BECAUSE IT EMITS, AND THAT IS THE WHOLE STORY OF Г24. Reading an input
            // WRITES the subgraph behind it: it advances `nextVar` and appends statements to `body`. It
            // used to be called `InputExpr`, which reads like a pure accessor, and thirteen emitter
            // branches consequently called it twice or three times inside ONE `std::format(...)`
            // argument list. C++ does not order function arguments: clang evaluates them left to right,
            // MSVC right to left. So the same `.dgraph` compiled to DIFFERENT GLSL on macOS and on
            // Windows — `n1 = <noise>; n2 = <density>; n0 = n2 * n1.x;` there against
            // `n1 = <density>; n2 = <noise>; n0 = n1 * n2.x;` here — which is not cosmetic: the emitted
            // text is hashed by Core::ShaderVariant::Hash(), that hash is mixed into the SPIR-V cache
            // key and into Graphic::CloudEnvironmentFingerprint, and the text itself is written to a
            // committed `.shader`. One graph, two machines, two artifacts.
            //
            // THE RULE THAT REPLACES IT: at most one call to this function per full expression. Every
            // caller that needs two inputs resolves them into named locals first, in pin order, because
            // consecutive statements ARE sequenced. `ShaderGraphDeterminism` enforces the rule over this
            // file's own text, so the shape cannot come back at the fourteenth branch.
            std::string EmitInput( const Node& node, size_t inputIndex, const char* fallback )
            {
                const Pin& pin = node.Inputs[inputIndex];
                auto       it  = linkIntoPin.find( pin.Id );
                if ( it == linkIntoPin.end() )
                    return fallback;

                const Node* src = nodeByPin.count( it->second ) ? nodeByPin.at( it->second ) : nullptr;
                if ( !src )
                    return fallback;
                const std::string var = EmitNode( *src );
                if ( !error.empty() )
                    return fallback;

                // Multi-output nodes: pick the component for the linked pin. Both texture nodes pair a
                // whole-sample pin with an R pin, and `.r` reads the same on the vec4 one and on the
                // medium's vec3 one — the difference between the nodes is where the coordinate comes
                // from, not what a sample is.
                if ( ( src->Kind == "TextureSample" || src->Kind == "MediumTexture" ) &&
                     src->Outputs.size() == 2 && it->second == src->Outputs[1].Id )
                    return var + ".r";

                // The Cloud Sample node hands out one struct member per output pin, and Split (Vector 3)
                // and Cloud Noise Volume one component per pin. All three are "one variable, several
                // fields", so the pin's INDEX picks the suffix rather than the node emitting seven
                // statements nobody reads.
                if ( src->Kind == "CloudSample" || src->Kind == "SplitVec3" || src->Kind == "CloudNoise" )
                {
                    for ( size_t i = 0; i < src->Outputs.size(); ++i )
                    {
                        if ( src->Outputs[i].Id != it->second )
                            continue;
                        if ( src->Kind == "SplitVec3" )
                            return var + "." + std::string( 1, "xyz"[i] );
                        // THE PIN'S INDEX IS THE VOLUME'S CHANNEL INDEX, and the channels are what
                        // Assets::CloudNoiseChannel says they are (the deck's own order, p.96): curly
                        // alligator low and high, then alligator low and high. The pin NAMES carry that
                        // meaning and a test in Desert/Tests/Editor/ShaderGraphCompiler reads them back
                        // out of CloudNoiseVolume.cpp, so a renamed channel is red here rather than a
                        // graph quietly reading the wrong frequency.
                        if ( src->Kind == "CloudNoise" )
                            return var + "." + std::string( 1, "xyzw"[i] );

                        // ONE PIN OF THIS NODE IS SCOPE-LIMITED, and by the same rule as Layer Albedo and
                        // Layer Occlusion above: it is emitted into five functions and it only MEANS
                        // anything in the two a shadow march calls. Elsewhere it is the literal zero, so
                        // a `mix( expensive, cheap, ShadowRay )` there is a branch that can never be
                        // taken — a knob that does nothing, delivered as a feature. Refused by name
                        // instead, with the outputs that do work listed, because the artist cannot see
                        // which functions their canvas is compiled into.
                        if ( src->Outputs[i].Name == "ShadowRay" && !ShadowRayIsInScope( currentOutput ) )
                        {
                            error = std::format(
                                 "the Cloud Sample node's 'ShadowRay' output is only meaningful in the {} "
                                 "output(s) of the Volume Output node ({}); it is reachable from '{}', "
                                 "where no shadow march ever asks the medium anything and the value is "
                                 "always zero",
                                 ShadowRayScopes().size(), ShadowRayScopeList(),
                                 currentOutput.empty() ? std::string( "<none>" ) : currentOutput );
                            return fallback;
                        }
                        return var + "." + src->Outputs[i].Name;
                    }
                }
                return var;
            }

            // Emits the node's statement once; returns its variable name.
            std::string EmitNode( const Node& node )
            {
                if ( auto it = varOf.find( &node ); it != varOf.end() )
                    return it->second;
                if ( state[&node] == 1 )
                {
                    error = std::format( "cycle detected at node '{}'", node.Kind );
                    return "vec4(0)";
                }
                state[&node] = 1;

                const std::string var  = std::format( "n{}", nextVar++ );
                std::string       decl;

                if ( node.Kind == "TextureSample" )
                {
                    const std::string uv = EmitInput( node, 0, "v_UV" );
                    decl = std::format( "vec4 {} = texture( {}, {} );", var, node.ParamName, uv );
                }
                else if ( node.Kind == "MediumTexture" )
                {
                    touchedParams.insert( &node );
                    // textureLod AND NOT texture, AND THE REASON IS THE LEVEL, NOT THE COMPILER. What used
                    // to be written here — that glslang REFUSES `texture()` in a compute stage because
                    // there are no derivatives — was measured false: mutating the ShaderCacheKey census's
                    // medium from `textureLod( s, uv, 0.0 )` to `texture( s, uv )` compiles clean in all
                    // four real programs under shaderc with SetWarningsAsErrors(), and the shipped
                    // CloudFetchNoise is a plain `texture(...)` in every one of them. What is actually
                    // true is that an implicit level is UNDEFINED outside a fragment stage, so a compute
                    // march would sample whichever mip the driver happens to pick. Level 0 is stated
                    // because a volume sample has no screen-space footprint to derive one from.
                    // Only .rgb: the RGB pin is a Vec3 (see the catalogue), because this domain has no
                    // vec4 sink to wire a fourth channel into.
                    const std::string u = EmitInput( node, 0, "0.0" );
                    const std::string v = EmitInput( node, 1, "0.0" );

                    decl = std::format( "vec3 {} = textureLod( {}, vec2( {}, {} ), 0.0 ).rgb;", var,
                                        node.ParamName, u, v );
                }
                else if ( node.Kind == "Vec3Param" )
                {
                    touchedParams.insert( &node );
                    decl = std::format( "vec3 {} = {}.{}.xyz;", var, ::Desert::Core::kCloudMediumInstanceName,
                                        node.ParamName );
                }
                else if ( node.Kind == "ColorParam" || node.Kind == "FloatParam" )
                {
                    touchedParams.insert( &node );
                    const char* type = node.Kind == "ColorParam" ? "vec4" : "float";
                    // TWO TRANSPORTS FOR ONE NODE, because the two domains keep their values in different
                    // places. A surface or post-process graph reads its own row of the shared `Materials[]`
                    // buffer, which the DSL's `Binding(n)` sugar declares and a push constant indexes. A
                    // medium is compiled into four COMPUTE programs that have neither, so it declares one
                    // block of its own in the reserved window — and every field of that block is a vec4,
                    // which is why a float reads `.x` here.
                    decl = doc.DomainEnum() == Domain::Volume
                                ? std::format( "{} {} = {}.{}.x;", type, var,
                                               ::Desert::Core::kCloudMediumInstanceName, node.ParamName )
                                : std::format( "{} {} = u_Material.{};", type, var, node.ParamName );
                }
                else if ( node.Kind == "CloudSample" )
                    decl = std::format( "CloudGraphSample {} = CloudGraphSampleAt( params, field, "
                                        "positionKm );",
                                        var );
                else if ( node.Kind == "CloudNoise" )
                {
                    // THE SLOT IS AN int ON THE OTHER SIDE OF THE MACRO and a float on this one, because
                    // the palette has a single numeric pin type and a second one would exist for this one
                    // value. Unwired it is the producer's own index, passed straight through with no
                    // arithmetic at all; wired it goes through CloudNoiseSlotOf, which rounds and clamps —
                    // truncation would push a slot carried through a Lerp down to the volume below it, and
                    // an out-of-range index would take the fetch's `else` branch and read as "my second
                    // cloud type stopped eroding".
                    const std::string slot =
                         IsLinked( node, 0 ) ? std::format( "CloudNoiseSlotOf( {} )", EmitInput( node, 0, "0.0" ) )
                                             : std::string( "field.NoiseSlot" );
                    // The macro, not a sampler: Common/CloudField.glslh keeps every fetch behind one so
                    // its maths can also be compiled as C++, and the medium is substituted INSIDE those
                    // guards — so CLOUD_SAMPLE_NOISE is defined at this point in all four consumers and a
                    // sampler named here would be a fifth declaration of volumes the scene already bound.
                    decl =
                         std::format( "vec4 {} = CLOUD_SAMPLE_NOISE( {}, {} );", var, slot,
                                      EmitInput( node, 1, "CloudDefaultNoiseCoordinate( params, positionKm )" ) );
                }
                else if ( node.Kind == "DefaultDensity" )
                    decl = std::format( "float {} = CloudDefaultDensity( params, field, positionKm );", var );
                else if ( node.Kind == "DefaultExtinctionFactor" )
                    decl = std::format( "float {} = CloudDefaultExtinctionFactor( params, field, positionKm );",
                                        var );
                else if ( node.Kind == "LayerAlbedo" || node.Kind == "LayerOcclusion" )
                {
                    // IN SCOPE IN EXACTLY ONE OUTPUT. See Compiler::currentOutput: the medium is five
                    // functions, and these two are arguments of one of them each.
                    const char* home = node.Kind == "LayerAlbedo" ? "Albedo" : "AmbientOcclusion";
                    if ( currentOutput != home )
                    {
                        error =
                             std::format( "node {} may only feed the '{}' output of the Volume Output node; it is "
                                          "reachable from '{}', where the layer's own value does not exist",
                                          NodeLabel( node ), home,
                                          currentOutput.empty() ? std::string( "<none>" ) : currentOutput );
                        decl = std::format( "float {} = 0.0;", var );
                    }
                    else if ( node.Kind == "LayerAlbedo" )
                        decl = std::format( "vec3 {} = materialAlbedo;", var );
                    else
                        decl = std::format( "float {} = ambientOcclusion;", var );
                }
                else if ( node.Kind == "CloudParam" )
                {
                    const VolumeParam* row = nullptr;
                    for ( const auto& candidate : VolumeParams() )
                        if ( node.ParamName == candidate.SchemaName )
                            row = &candidate;
                    if ( !row )
                    {
                        // ValidateGraph names this first and better; reaching here means the two lists
                        // disagreed, which is worth saying out loud rather than emitting a black.
                        error = std::format( "node {} reads cloud material property '{}', which the Volume "
                                             "domain does not expose",
                                             NodeLabel( node ), node.ParamName );
                        decl  = std::format( "float {} = 0.0;", var );
                    }
                    else
                        decl = std::format( "float {} = {};", var, row->Expression );
                }
                else if ( node.Kind == "Vec3Const" )
                    decl = std::format( "vec3 {} = vec3( {}, {}, {} );", var, Lit( node.Value[0] ),
                                        Lit( node.Value[1] ), Lit( node.Value[2] ) );
                else if ( node.Kind == "MultiplyVec3" )
                {
                    const std::string a = EmitInput( node, 0, "vec3( 1.0 )" );
                    const std::string b = EmitInput( node, 1, "vec3( 1.0 )" );

                    decl = std::format( "vec3 {} = {} * {};", var, a, b );
                }
                else if ( node.Kind == "ScaleVec3" )
                {
                    const std::string value = EmitInput( node, 0, "vec3( 1.0 )" );
                    const std::string scale = EmitInput( node, 1, "1.0" );

                    decl = std::format( "vec3 {} = {} * {};", var, value, scale );
                }
                else if ( node.Kind == "SplitVec3" )
                    decl = std::format( "vec3 {} = {};", var, EmitInput( node, 0, "vec3( 0.0 )" ) );
                else if ( node.Kind == "AddFloat" )
                {
                    const std::string a = EmitInput( node, 0, "0.0" );
                    const std::string b = EmitInput( node, 1, "0.0" );

                    decl = std::format( "float {} = {} + {};", var, a, b );
                }
                else if ( node.Kind == "SaturateFloat" )
                    decl = std::format( "float {} = clamp( {}, 0.0, 1.0 );", var, EmitInput( node, 0, "0.0" ) );
                else if ( node.Kind == "PowerFloat" )
                {
                    const std::string base     = EmitInput( node, 0, "0.0" );
                    const std::string exponent = EmitInput( node, 1, "1.0" );

                    decl = std::format( "float {} = pow( max( {}, 0.0 ), {} );", var, base, exponent );
                }
                else if ( node.Kind == "LerpFloat" )
                {
                    const std::string from = EmitInput( node, 0, "0.0" );
                    const std::string to   = EmitInput( node, 1, "1.0" );
                    const std::string t    = EmitInput( node, 2, "0.5" );

                    decl = std::format( "float {} = mix( {}, {}, {} );", var, from, to, t );
                }
                else if ( node.Kind == "ColorConst" )
                    decl = std::format( "vec4 {} = {};", var, Vec4Lit( node.Value ) );
                else if ( node.Kind == "FloatConst" )
                    decl = std::format( "float {} = {};", var, Lit( node.Value[0] ) );
                else if ( node.Kind == "SceneColor" )
                    decl = std::format( "vec4 {} = texture( u_SceneTexture, v_UV );", var );
                else if ( node.Kind == "UV" )
                    decl = std::format( "vec2 {} = v_UV;", var );
                else if ( node.Kind == "TileUV" )
                {
                    const std::string uv     = EmitInput( node, 0, "v_UV" );
                    const std::string tiling = EmitInput( node, 1, "1.0" );

                    decl = std::format( "vec2 {} = {} * {};", var, uv, tiling );
                }
                else if ( node.Kind == "Multiply" )
                {
                    const std::string a = EmitInput( node, 0, "vec4( 1.0 )" );
                    const std::string b = EmitInput( node, 1, "vec4( 1.0 )" );

                    decl = std::format( "vec4 {} = {} * {};", var, a, b );
                }
                else if ( node.Kind == "Scale" )
                {
                    const std::string value = EmitInput( node, 0, "vec4( 1.0 )" );
                    const std::string scale = EmitInput( node, 1, "1.0" );

                    decl = std::format( "vec4 {} = {} * {};", var, value, scale );
                }
                else if ( node.Kind == "Add" )
                {
                    const std::string a = EmitInput( node, 0, "vec4( 0.0 )" );
                    const std::string b = EmitInput( node, 1, "vec4( 0.0 )" );

                    decl = std::format( "vec4 {} = {} + {};", var, a, b );
                }
                else if ( node.Kind == "Lerp" )
                {
                    const std::string from = EmitInput( node, 0, "vec4( 0.0 )" );
                    const std::string to   = EmitInput( node, 1, "vec4( 1.0 )" );
                    const std::string t    = EmitInput( node, 2, "0.5" );

                    decl = std::format( "vec4 {} = mix( {}, {}, {} );", var, from, to, t );
                }
                else if ( node.Kind == "OneMinus" )
                    decl = std::format( "vec4 {} = vec4( 1.0 ) - {};", var, EmitInput( node, 0, "vec4( 0.0 )" ) );
                else if ( node.Kind == "MultiplyFloat" )
                {
                    const std::string a = EmitInput( node, 0, "1.0" );
                    const std::string b = EmitInput( node, 1, "1.0" );

                    decl = std::format( "float {} = {} * {};", var, a, b );
                }
                else if ( node.Kind == "Saturate" )
                    decl = std::format( "vec4 {} = clamp( {}, vec4( 0.0 ), vec4( 1.0 ) );", var,
                                        EmitInput( node, 0, "vec4( 0.0 )" ) );
                else if ( node.Kind == "Power" )
                {
                    const std::string base     = EmitInput( node, 0, "vec4( 0.0 )" );
                    const std::string exponent = EmitInput( node, 1, "1.0" );

                    decl = std::format( "vec4 {} = pow( max( {}, vec4( 0.0 ) ), vec4( {} ) );", var, base,
                                        exponent );
                }
                else if ( node.Kind == "Sine" )
                    decl = std::format( "float {} = sin( {} );", var, EmitInput( node, 0, "0.0" ) );
                else if ( node.Kind == "Time" )
                    decl = std::format( "float {} = timeUB.TimeData.x;", var );
                else
                {
                    // ValidateGraph has already rejected kinds that are not in the catalogue, so
                    // reaching here means the OPPOSITE: a NodeSpec was added without an emitter
                    // branch. Saying so beats emitting a silent black.
                    error = std::format( "node kind '{}' is in the palette but has no compiler rule", node.Kind );
                    decl  = std::format( "vec4 {} = vec4( 0.0 );", var );
                }

                body << "            " << decl << "\n";
                varOf[&node] = var;
                state[&node] = 2;
                return var;
            }
        };

        bool IsValidIdentifier( const std::string& s )
        {
            if ( s.empty() || ( !std::isalpha( (unsigned char)s[0] ) && s[0] != '_' ) )
                return false;
            return std::all_of( s.begin(), s.end(),
                                []( unsigned char c ) { return std::isalnum( c ) || c == '_'; } );
        }

        // ------------------------------------------------------------ validation ---------------
        // The GLSL type a pin carries. This is the whole point of the checks below: the compiler
        // emits `float`/`vec2`/`vec4` declarations straight from the node kind, so two pins that
        // disagree here become a GLSL type error in generated code.
        const char* GlslTypeName( ValueType t )
        {
            switch ( t )
            {
                case ValueType::Float:
                    return "float";
                case ValueType::Vec2:
                    return "vec2";
                case ValueType::Color:
                    return "vec4";
                case ValueType::Vec3:
                    return "vec3";
            }
            return "<unknown>";
        }

        const char* DomainName( Domain d )
        {
            switch ( d )
            {
                case Domain::PostProcess:
                    return "Post Process";
                case Domain::Volume:
                    return "Cloud Medium";
                case Domain::Surface:
                    break;
            }
            return "Surface";
        }

        // How the node reads on the canvas — its palette title, plus the parameter name when it has
        // one. Every diagnostic below is phrased in these terms so the artist is pointed at a node
        // they can see and click, not at a line of a file they never wrote.
        std::string NodeLabel( const Node& node )
        {
            const NodeSpec*   spec  = FindSpec( node.Kind );
            const std::string title = spec ? spec->Title : node.Kind;
            return node.ParamName.empty() ? std::format( "'{}'", title )
                                          : std::format( "'{}' ('{}')", title, node.ParamName );
        }

        // Where a pin id lives. Built once so a link can be resolved to (node, side, index) and
        // reported by name.
        struct PinRef
        {
            const Node* Owner = nullptr;
            bool        Input = false;
            size_t      Index = 0;
            ValueType   Type  = ValueType::Float;
        };

        // Full structural + type check of a graph document, run BEFORE a single line is emitted.
        //
        // The canvas already refuses a mismatched link while the artist drags it (NodeGraphPanel's
        // ed::QueryNewLink compares Pin::Type), but a .dgraph is plain JSON: one written by hand, by
        // a script, or by an older build reaches the compiler with none of that enforcement. Until
        // this function existed such a graph compiled happily and the mismatch surfaced from shaderc
        // as e.g. "MatBroken.shader:25: error: '=' : cannot convert from 'vec2' to 'vec4'" — a line
        // number in GENERATED code, naming neither the node nor the link that caused it.
        //
        // Node identity only exists at this level; by the time the text is emitted the nodes have
        // become n0, n1, n2. So this is the last place an error can name what the artist drew.
        //
        // Returns an empty string when the document is well-formed.
        std::string ValidateGraph( const Document& doc, Domain domain )
        {
            // ---- nodes: known kind, offered in this domain, pins agreeing with the catalogue ----
            for ( const auto& node : doc.Nodes )
            {
                const NodeSpec* spec = FindSpec( node.Kind );
                if ( !spec )
                    return std::format( "node id {} has unknown kind '{}'", node.Id, node.Kind );

                if ( !SpecInDomain( *spec, domain ) )
                    return std::format( "node {} is not available in the {} domain", NodeLabel( node ),
                                        DomainName( domain ) );

                // THE BAKE/MARCH SPLIT, MADE UNEXPRESSIBLE. About half of the cloud material's values are
                // inputs to a CPU bake that runs for seconds and produces the volume this graph reads the
                // RESULT of; they are not in the shader's scope at all. Refused BY NAME here, listing what
                // is readable, rather than by an empty palette entry or a GLSL error in generated code.
                if ( node.Kind == "CloudParam" )
                {
                    const bool readable =
                         std::any_of( VolumeParams().begin(), VolumeParams().end(),
                                      [&node]( const VolumeParam& p ) { return node.ParamName == p.SchemaName; } );
                    if ( !readable )
                    {
                        std::string readableNames;
                        for ( const auto& p : VolumeParams() )
                            readableNames += ( readableNames.empty() ? "" : ", " ) + std::string( p.SchemaName );
                        return std::format(
                             "node {} reads cloud material property '{}', which a medium cannot see. Most "
                             "of that material is baked on the CPU before the march runs; what a graph "
                             "may read is: {}",
                             NodeLabel( node ), node.ParamName, readableNames );
                    }
                }

                // A node whose pin list disagrees with its kind is the crash case, not just a bad
                // message: the emitter indexes node.Inputs[i] positionally for every kind it knows,
                // so a hand-trimmed "Inputs": [] on a Multiply used to read off the end of the vector.
                if ( node.Inputs.size() != spec->Inputs.size() || node.Outputs.size() != spec->Outputs.size() )
                    return std::format(
                         "node {} has {} input(s) and {} output(s), but kind '{}' declares {} and {}",
                         NodeLabel( node ), node.Inputs.size(), node.Outputs.size(), node.Kind,
                         spec->Inputs.size(), spec->Outputs.size() );

                // Pin::Type in the file is a serialized MIRROR of the catalogue, and the canvas
                // type-checks against that mirror. If the two disagree the file can make the canvas
                // accept a link the compiler cannot emit, so the mirror is checked rather than trusted.
                //
                // AND SO IS Pin::Name, BECAUSE THE NAME IS NOT DECORATION — IT IS EMITTED. The Cloud
                // Sample node hands out one struct member per output pin, and the emitter writes
                // `<var>.<name>` with the name taken FROM THE DOCUMENT; the ShadowRay scope refusal
                // matches on that same stored string. Until this loop compared names, a .dgraph whose
                // pin had been renamed to something of the right type passed here untouched and then
                // failed one of two ways: it reached shaderc and died on a line of GENERATED code —
                // the exact failure this function's own comment above promises to prevent — or, when
                // the new name happened to be another member of the struct, it compiled into a
                // silently different value with the scope check never firing.
                //
                // REFUSED RATHER THAN REPAIRED, which is the answer MigrateToCatalogue already gives:
                // a stored pin list that is not a PREFIX of the catalogue is left exactly as it is
                // there rather than guessed at. A renamed pin is either a corrupt document or one
                // written against a different catalogue, and substituting the catalogue's name for it
                // would change what the graph MEANS with nothing to show for it. A catalogue rename
                // travels as an explicit migration over the files, like every other format change here.
                for ( size_t i = 0; i < node.Inputs.size(); ++i )
                {
                    if ( node.Inputs[i].Name != spec->Inputs[i].Name )
                        return std::format( "node {}: input {} is named '{}' but kind '{}' declares '{}' at "
                                            "that position",
                                            NodeLabel( node ), i, node.Inputs[i].Name, node.Kind,
                                            spec->Inputs[i].Name );
                    if ( node.Inputs[i].Type != static_cast<int>( spec->Inputs[i].Type ) )
                        return std::format( "node {}: input '{}' is stored as {} but kind '{}' declares it {}",
                                            NodeLabel( node ), node.Inputs[i].Name,
                                            GlslTypeName( static_cast<ValueType>( node.Inputs[i].Type ) ),
                                            node.Kind, GlslTypeName( spec->Inputs[i].Type ) );
                }

                for ( size_t i = 0; i < node.Outputs.size(); ++i )
                {
                    if ( node.Outputs[i].Name != spec->Outputs[i].Name )
                        return std::format( "node {}: output {} is named '{}' but kind '{}' declares '{}' at "
                                            "that position",
                                            NodeLabel( node ), i, node.Outputs[i].Name, node.Kind,
                                            spec->Outputs[i].Name );
                    if ( node.Outputs[i].Type != static_cast<int>( spec->Outputs[i].Type ) )
                        return std::format( "node {}: output '{}' is stored as {} but kind '{}' declares it {}",
                                            NodeLabel( node ), node.Outputs[i].Name,
                                            GlslTypeName( static_cast<ValueType>( node.Outputs[i].Type ) ),
                                            node.Kind, GlslTypeName( spec->Outputs[i].Type ) );
                }
            }

            // ---- pin index, rejecting duplicate ids ----
            std::unordered_map<uint64_t, PinRef> pins;
            for ( const auto& node : doc.Nodes )
            {
                const NodeSpec* spec = FindSpec( node.Kind );
                const auto      add  = [&]( const Pin& pin, bool input, size_t index ) -> std::string
                {
                    if ( pin.Id == 0 )
                        return std::format( "node {}: pin '{}' has no id", NodeLabel( node ), pin.Name );
                    const ValueType type = input ? spec->Inputs[index].Type : spec->Outputs[index].Type;
                    auto [it, fresh]     = pins.emplace( pin.Id, PinRef{ &node, input, index, type } );
                    if ( !fresh )
                        return std::format( "pin id {} is used by both node {} and node {}", pin.Id,
                                            NodeLabel( *it->second.Owner ), NodeLabel( node ) );
                    return {};
                };
                for ( size_t i = 0; i < node.Inputs.size(); ++i )
                    if ( std::string err = add( node.Inputs[i], true, i ); !err.empty() )
                        return err;
                for ( size_t i = 0; i < node.Outputs.size(); ++i )
                    if ( std::string err = add( node.Outputs[i], false, i ); !err.empty() )
                        return err;
            }

            // ---- links: both ends real, output -> input, one link per input, types equal ----
            std::unordered_set<uint64_t> takenInputs;
            for ( const auto& link : doc.Links )
            {
                auto from = pins.find( link.From );
                auto to   = pins.find( link.To );
                if ( from == pins.end() )
                    return std::format( "link {} starts at pin id {}, which no node owns", link.Id, link.From );
                if ( to == pins.end() )
                    return std::format( "link {} ends at pin id {}, which no node owns", link.Id, link.To );

                if ( from->second.Input )
                    return std::format( "link {} starts at input '{}' of node {} — a link must start at "
                                        "an output",
                                        link.Id, from->second.Owner->Inputs[from->second.Index].Name,
                                        NodeLabel( *from->second.Owner ) );
                if ( !to->second.Input )
                    return std::format( "link {} ends at output '{}' of node {} — a link must end at an "
                                        "input",
                                        link.Id, to->second.Owner->Outputs[to->second.Index].Name,
                                        NodeLabel( *to->second.Owner ) );

                // Two links into one input: the emitter keeps whichever the map saw last, so the
                // artist's picture and the generated code disagree with nothing to show for it.
                if ( !takenInputs.insert( link.To ).second )
                    return std::format( "input '{}' of node {} has more than one link into it",
                                        to->second.Owner->Inputs[to->second.Index].Name,
                                        NodeLabel( *to->second.Owner ) );

                // The rule is exactly the canvas's: equal types, no implicit conversion. Stating it
                // twice in two places is the risk here, so both sides compare the SAME catalogue
                // types — the canvas via Pin::Type, checked against the catalogue above.
                if ( from->second.Type != to->second.Type )
                    return std::format( "cannot link output '{}' of node {} ({}) into input '{}' of node {} ({}): "
                                        "types do not match",
                                        from->second.Owner->Outputs[from->second.Index].Name,
                                        NodeLabel( *from->second.Owner ), GlslTypeName( from->second.Type ),
                                        to->second.Owner->Inputs[to->second.Index].Name,
                                        NodeLabel( *to->second.Owner ), GlslTypeName( to->second.Type ) );
            }

            return {};
        }
    } // namespace

    Common::ResultStr<std::string> CompileToDShader( const Document& doc )
    {
        if ( !IsValidIdentifier( doc.Name ) )
            return Common::MakeError<std::string>(
                 std::format( "'{}' is not a valid shader name (letters/digits/underscore)", doc.Name ) );

        const Domain      domain   = doc.DomainEnum();
        const char* const outKind  = OutputKind( domain );
        const NodeSpec*   outSpec  = FindSpec( outKind );
        const std::string outTitle = outSpec ? outSpec->Title : outKind;

        // Structure and types first: everything below indexes pins positionally and emits typed GLSL
        // declarations, both of which assume a well-formed document.
        if ( std::string err = ValidateGraph( doc, domain ); !err.empty() )
            return Common::MakeError<std::string>( std::move( err ) );

        const Node* output = nullptr;
        for ( const auto& node : doc.Nodes )
        {
            if ( node.Kind != outKind )
                continue;
            if ( output )
                return Common::MakeError<std::string>(
                     std::format( "graph has more than one {}", outTitle ) );
            output = &node;
        }
        if ( !output )
            return Common::MakeError<std::string>( std::format( "graph needs a {} node", outTitle ) );

        // ---------------------------------------------------------- Volume domain -----------------
        //
        // A PROGRAM FRAGMENT AND NOT A PROGRAM. The medium is compiled INTO the four shipped programs
        // that sample the cloud field, as the substitution for one of their includes, so what is emitted
        // here is a `Medium { ... }` block: no stages, no State and no vertex contract. It MAY carry a
        // Properties block — the medium's own parameters and images — and that block is pure schema: the
        // DSL's `Binding()/TextureBinding()` sugar declares a `Materials[]` row indexed by a push constant
        // no compute program here has, so the declarations are written into the medium body below instead,
        // at the numbers Core::kCloudMedium*Binding reserve.
        //
        // FIVE FUNCTIONS, EACH COMPILED SEPARATELY. Every output pin gets its own Compiler, so a node is
        // emitted only into the function that actually reads it — the alternative, one body shared by
        // five returns, would evaluate the whole graph five times per sample and make the two
        // scope-limited nodes (Layer Albedo, Layer Occlusion) impossible to police.
        //
        // AN UNCONNECTED PIN IS THE SHIPPED VALUE, NOT ZERO. That is what makes a half-authored graph a
        // modification of the sky rather than a deletion of it, and it is why a Volume Output node with
        // nothing wired into it compiles to five forwards that are byte-for-byte the default medium.
        if ( domain == Domain::Volume )
        {
            struct MediumFunction
            {
                const char* Signature;
                const char* Pin;      // the Volume Output input it is compiled from
                const char* Fallback; // what an unconnected pin emits: the shipped default, never a zero
            };

            // clang-format off
            static const MediumFunction kFunctions[] = {
                { "float CloudSampleDensity( CloudFieldParams params, CloudFieldSample field, vec3 positionKm )",
                  "Density", "CloudDefaultDensity( params, field, positionKm )" },
                { "float CloudSampleExtinctionFactor( CloudFieldParams params, CloudFieldSample field, vec3 positionKm )",
                  "Extinction", "CloudDefaultExtinctionFactor( params, field, positionKm )" },
                { "vec3 CloudSampleAlbedo( CloudFieldParams params, CloudFieldSample field, vec3 positionKm, vec3 materialAlbedo )",
                  "Albedo", "CloudDefaultAlbedo( params, field, positionKm, materialAlbedo )" },
                { "vec3 CloudSampleEmissive( CloudFieldParams params, CloudFieldSample field, vec3 positionKm )",
                  "Emissive", "CloudDefaultEmissive( params, field, positionKm )" },
                { "float CloudSampleOcclusion( CloudFieldParams params, CloudFieldSample field, vec3 positionKm, float ambientOcclusion )",
                  "AmbientOcclusion", "CloudDefaultOcclusion( params, field, positionKm, ambientOcclusion )" },
            };
            // clang-format on

            // THE FIVE BODIES FIRST, THE DECLARATIONS AFTER, and the order is the mechanism rather than a
            // tidiness. Which properties this medium HAS is the set of property nodes the five functions
            // actually reached (see Compiler::touchedParams), so the bodies have to exist before the block
            // that declares them can be written.
            std::ostringstream              bodies;
            std::unordered_set<const Node*> touched;

            for ( const auto& function : kFunctions )
            {
                // The pin is found by NAME rather than by index, because the two lists are maintained in
                // different files: the catalogue's VolumeOutput spec and the table above. A rename in one
                // is a named refusal here instead of five functions silently compiled from the wrong pins.
                size_t pinIndex = output->Inputs.size();
                for ( size_t i = 0; i < output->Inputs.size(); ++i )
                    if ( output->Inputs[i].Name == function.Pin )
                        pinIndex = i;
                if ( pinIndex == output->Inputs.size() )
                    return Common::MakeError<std::string>(
                         std::format( "the Volume Output node has no '{}' input, so the medium's '{}' "
                                      "function cannot be compiled",
                                      function.Pin, function.Signature ) );

                Compiler compiler( doc );
                compiler.currentOutput       = function.Pin;
                const std::string expression = compiler.EmitInput( *output, pinIndex, function.Fallback );
                if ( !compiler.error.empty() )
                    return Common::MakeError<std::string>( compiler.error );

                touched.insert( compiler.touchedParams.begin(), compiler.touchedParams.end() );

                bodies << "        " << function.Signature << "\n        {\n";
                bodies << compiler.body.str();
                bodies << std::format( "            return {};\n", expression );
                bodies << "        }\n\n";
            }

            // IN DOCUMENT ORDER, NOT IN THE ORDER THE FIVE TRAVERSALS HAPPENED TO REACH THEM. The order
            // here IS the layout: the runtime packs one vec4 per numeric property and binds one sampler
            // per image, both in the order the Properties block declares. A traversal order would make the
            // packing depend on which output pin the artist wired first.
            std::vector<const Node*>        mediumValues;   // Color and Float properties, this order
            std::vector<const Node*>        mediumTextures; // Texture2D properties
            std::unordered_set<std::string> seenMedium;
            for ( const auto& node : doc.Nodes )
            {
                if ( !touched.count( &node ) )
                    continue;
                if ( !IsValidIdentifier( node.ParamName ) )
                    return Common::MakeError<std::string>(
                         std::format( "'{}' is not a valid parameter name", node.ParamName ) );
                if ( !seenMedium.insert( node.ParamName ).second )
                    continue; // same name used twice = one property, as in every other domain
                if ( node.Kind == "MediumTexture" )
                    mediumTextures.push_back( &node );
                else
                    mediumValues.push_back( &node );
            }

            if ( mediumValues.size() > ::Desert::Core::kCloudMediumMaxValues )
                return Common::MakeError<std::string>( std::format(
                     "this medium declares {} exposed values and a cloud medium may declare at most {}. "
                     "The buffer that carries them is allocated once, before any medium exists, because a "
                     "device allocation inside the frame has nowhere to report a failure",
                     mediumValues.size(), ::Desert::Core::kCloudMediumMaxValues ) );

            if ( mediumTextures.size() > ::Desert::Core::kCloudMediumMaxTextures )
                return Common::MakeError<std::string>( std::format(
                     "this medium declares {} Medium Texture properties and a cloud medium may declare at "
                     "most {}. Every one of them is a descriptor that all four programs sampling the cloud "
                     "field have to bind on every frame of every scene, with or without clouds in it",
                     mediumTextures.size(), ::Desert::Core::kCloudMediumMaxTextures ) );

            std::ostringstream out;
            out << "// GENERATED by the Desert Shader Graph editor — edit the .dgraph, not this file.\n";
            out << "Shader \"" << doc.Name << "\"\n{\n    Domain Volume\n\n";

            // THE SCHEMA, AND ONLY THE SCHEMA. No Binding()/TextureBinding(): the sugar those options turn
            // on declares a row of the shared `Materials[]` buffer addressed through a push constant, which
            // is the mesh path's transport and does not exist in any of the four compute programs a medium
            // is compiled into. What this block is FOR is being read back — by the material window, which
            // draws a row per entry, and by Graphic::BuildCloudMediumValues, which packs the buffer from it.
            if ( !mediumValues.empty() || !mediumTextures.empty() )
            {
                out << "    Properties\n    {\n";
                // Timing(Immediate) ON EVERY ROW, and it is a fact rather than a decoration: a medium is
                // GLSL the march runs per sample, so nothing declared here can be an input to the CPU bake
                // — the one thing in this material that costs seconds. The material window draws that
                // claim, and the Volume domain refuses a `Timing(Rebake)` property to a graph by name.
                for ( const auto* n : mediumValues )
                {
                    if ( n->Kind == "Vec3Param" )
                        out << std::format( "        Color3    {} (\"{}\", Category(\"{}\"), Timing(Immediate)) = "
                                            "({}, {}, {})\n",
                                            n->ParamName, n->ParamName, kMediumCategory,
                                            Compiler::Lit( n->Value[0] ), Compiler::Lit( n->Value[1] ),
                                            Compiler::Lit( n->Value[2] ) );
                    else
                        out << std::format(
                             "        Float     {} (\"{}\", Category(\"{}\"), Timing(Immediate)) = {}\n",
                             n->ParamName, n->ParamName, kMediumCategory, Compiler::Lit( n->Value[0] ) );
                }
                for ( const auto* n : mediumTextures )
                    out << std::format( "        Texture2D {} (\"{}\", Category(\"{}\"), Timing(Immediate))\n",
                                        n->ParamName, n->ParamName, kMediumCategory );
                out << "    }\n\n";
            }

            out << "    Medium\n    {\n";
            // The shipped bodies, so every fallback above and every Default node below resolves. It is an
            // ordinary include of an ordinary header: this text is substituted for
            // Generated/CloudMedium.glslh, and Common/CloudMediumDefault.glslh is never substituted.
            out << "        #include <Common/CloudMediumDefault.glslh>\n\n";

            // EVERY FIELD IS A vec4, INCLUDING THE FLOATS. std430 would pad a float to a 16-byte slot
            // anyway, so nothing is spent — and what is bought is that the packing rule on the C++ side is
            // "one vec4 per property, in schema order" with no padding arithmetic to mirror. A padding rule
            // written on both sides of a seam is exactly the mirror this subsystem turned grey once.
            if ( !mediumValues.empty() )
            {
                out << std::format( "        struct {}\n        {{\n", ::Desert::Core::kCloudMediumStructName );
                for ( const auto* n : mediumValues )
                    out << std::format( "            vec4 {};\n", n->ParamName );
                out << "        };\n";
                out << std::format( "        layout( std430, binding = {} ) readonly buffer {}\n"
                                    "        {{\n            {} {};\n        }};\n\n",
                                    ::Desert::Core::kCloudMediumParamsBinding,
                                    ::Desert::Core::kCloudMediumBlockName, ::Desert::Core::kCloudMediumStructName,
                                    ::Desert::Core::kCloudMediumInstanceName );
            }
            for ( std::size_t i = 0; i < mediumTextures.size(); ++i )
                out << std::format( "        layout( binding = {} ) uniform sampler2D {};\n",
                                    ::Desert::Core::kCloudMediumTextureFirst + static_cast<uint32_t>( i ),
                                    mediumTextures[i]->ParamName );
            if ( !mediumTextures.empty() )
                out << "\n";

            out << bodies.str();
            out << "    }\n}\n";
            return Common::MakeSuccess( out.str() );
        }

        // Exposed properties: dedupe by name, validate identifiers.
        std::vector<const Node*> textures, colorParams, floatParams;
        {
            std::unordered_set<std::string> seen;
            for ( const auto& node : doc.Nodes )
            {
                const NodeSpec* spec = FindSpec( node.Kind );
                if ( !spec || !spec->HasParamName )
                    continue;
                if ( !IsValidIdentifier( node.ParamName ) )
                    return Common::MakeError<std::string>(
                         std::format( "'{}' is not a valid parameter name", node.ParamName ) );
                if ( !seen.insert( node.ParamName ).second )
                    continue; // same param used twice = same property, fine
                if ( node.Kind == "TextureSample" )
                    textures.push_back( &node );
                else if ( node.Kind == "ColorParam" )
                    colorParams.push_back( &node );
                else if ( node.Kind == "FloatParam" )
                    floatParams.push_back( &node );
            }
        }

        Compiler compiler( doc );
        std::string albedo, emission, alpha, sceneOut;
        std::string metallic, roughness, occlusion;
        if ( domain == Domain::PostProcess )
        {
            sceneOut = compiler.EmitInput( *output, 0, "vec4( 0.0 )" );
        }
        else
        {
            albedo   = compiler.EmitInput( *output, 0, "vec4( 0.8, 0.8, 0.8, 1.0 )" );
            emission = compiler.EmitInput( *output, 1, "vec4( 0.0 )" );
            alpha    = compiler.EmitInput( *output, 2, "1.0" );
            // Only when the surface is lit: an unlit graph has no shading model to feed, and asking
            // for these would emit the nodes behind them into a shader that never reads the result.
            // The fallbacks are the schema defaults of the standard material (StaticMeshPBR's
            // Properties block), so an unwired Metallic/Roughness/Occlusion pin and an untouched
            // PBR material describe the same surface.
            if ( doc.Lit )
            {
                metallic  = compiler.EmitInput( *output, 3, "0.0" );
                roughness = compiler.EmitInput( *output, 4, "0.5" );
                occlusion = compiler.EmitInput( *output, 5, "1.0" );
            }
        }
        if ( !compiler.error.empty() )
            return Common::MakeError<std::string>( compiler.error );

        const bool usesTime = std::any_of( doc.Nodes.begin(), doc.Nodes.end(),
                                           []( const Node& n ) { return n.Kind == "Time"; } );

        std::ostringstream out;
        out << "// GENERATED by the Desert Shader Graph editor — edit the .dgraph, not this file.\n";
        out << "Shader \"" << doc.Name << "\"\n{\n    Domain "
            << ( domain == Domain::PostProcess ? "PostProcess" : "Surface" ) << "\n\n";

        // Exposed properties block — shared across domains (post-process effects can expose params too).
        // Scene texture (post-process) sits at set 0 / binding 0, so params start at Binding(1).
        //
        // The graph's own textures are numbered from kGraphTextureBinding UPWARD, one per texture, and
        // that base is the first slot of the window reserved for graph-owned resources
        // (Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp). It used to be 2, which was safe only
        // while the engine blocks a graph could receive were two; a lit surface grew to twelve, so the
        // third texture in a graph would have landed on top of LightsMetadata. Nothing would have said
        // so: two GLSL declarations at one binding is a descriptor the engine writes twice and a shader
        // that reads whichever it got.
        //
        // THE SLOT LIST THAT USED TO BE WRITTEN OUT HERE IS GONE ON PURPOSE: it was prose asserting a
        // property of a tree that moves, and the next binding added to a shader-graph surface would have
        // left it wrong and silent. What keeps the window free now is a measurement over the compiled
        // SPIR-V of every shipped pass (Desert/Tests/Engine/ShaderCacheKey), which no comment can go
        // stale against.
        if ( !textures.empty() || !colorParams.empty() || !floatParams.empty() )
        {
            out << "    Properties";
            if ( !colorParams.empty() || !floatParams.empty() )
                out << " Binding(1)";
            if ( !textures.empty() )
                out << std::format( " TextureBinding({})", kGraphTextureBinding );
            out << "\n    {\n";
            for ( const auto* n : colorParams )
                out << std::format( "        Color     {} (\"{}\") = ({}, {}, {}, {})\n", n->ParamName,
                                    n->ParamName, Compiler::Lit( n->Value[0] ), Compiler::Lit( n->Value[1] ),
                                    Compiler::Lit( n->Value[2] ), Compiler::Lit( n->Value[3] ) );
            for ( const auto* n : floatParams )
                out << std::format( "        Float     {} (\"{}\") = {}\n", n->ParamName, n->ParamName,
                                    Compiler::Lit( n->Value[0] ) );
            for ( const auto* n : textures )
                out << std::format( "        Texture2D {} (\"{}\")\n", n->ParamName, n->ParamName );
            out << "    }\n\n";
        }

        // ---------------------------------------------------------- PostProcess domain ------------
        // Full-screen triangle over the rendered scene color; no mesh, no normals, no depth pass.
        if ( domain == Domain::PostProcess )
        {
            out << "    State\n    {\n        Cull None\n        ZTest Always\n        ZWrite Off\n    }\n\n";

            out << "    Vertex\n    {\n";
            out << "        #include <Common/QuadPositions.glslh>\n";
            out << "        #include <Common/QuadTextureCoords.glslh>\n\n";
            out << "        layout( location = 0 ) out vec2 v_UV;\n\n";
            out << "        void main()\n        {\n";
            out << "            v_UV        = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];\n";
            out << "            gl_Position = vec4( QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0 );\n";
            out << "        }\n    }\n\n";

            out << "    Fragment\n    {\n";
            out << "        layout( location = 0 ) in vec2 v_UV;\n";
            out << "        layout( location = 0 ) out vec4 o_Color;\n";
            out << "        layout( set = 0, binding = 0 ) uniform sampler2D u_SceneTexture;\n";
            if ( usesTime )
                out << "\n        #include <Common/TimeUB.glslh>\n";
            out << "\n        void main()\n        {\n";
            out << compiler.body.str();
            out << std::format( "            o_Color = {};\n", sceneOut );
            out << "        }\n    }\n";
            out << "}\n";
            return Common::MakeSuccess( out.str() );
        }

        // ---------------------------------------------------------- Surface domain ----------------
        out << "    State\n    {\n        Cull Back\n        ZTest LEqual\n        ZWrite On\n    }\n\n";

        // NO GLSL boilerplate lives in this compiler: the vertex contract and the engine-filled UB
        // declarations are shared .glslh includes (Resources/Shaders/Common/), configured with
        // defines — hand-written shaders reuse the same files. The generated file only contains the
        // structure and the graph's own fragment expressions.
        out << "    Vertex\n    {\n";
        if ( doc.Lit )
            out << "        #define GRAPH_LIT 1\n";
        out << "        #include <Common/GraphVertex.glslh>\n";
        out << "    }\n\n";

        out << "    Fragment\n    {\n";
        out << "        layout( location = 0 ) in vec2 v_UV;\n";
        if ( doc.Lit )
        {
            out << "        layout( location = 1 ) in vec3 v_Normal;\n";
            out << "        layout( location = 2 ) in vec3 v_WorldPos;\n";
            out << "        layout( location = 3 ) in vec3 v_CameraPos;\n";
        }
        out << "        layout( location = 0 ) out vec4 o_Color;\n";
        if ( usesTime )
            out << "\n        #include <Common/TimeUB.glslh>\n";
        // THE shading model, and the generator writes not one line of it. Everything a lit surface
        // needs — the bindings, the ambient, the sun, the punctual lights and the cloud shadow — is
        // behind this include, which is itself only calls into the engine's shared lighting texts.
        // What stood here instead was a formula of this compiler's own: a flat vec3( 0.12 ) ambient,
        // a Lambert cosine that did not divide albedo by PI, and no cloud shadow at all — three
        // defects the engine had already fixed in the texts this now calls.
        if ( doc.Lit )
            out << "\n        #include <Common/GraphSurfaceLighting.glslh>\n";
        out << "\n        void main()\n        {\n";
        out << compiler.body.str();
        out << std::format( "            vec4 albedo = {};\n", albedo );
        if ( doc.Lit )
        {
            out << "            vec3 N = normalize( v_Normal );\n";
            out << "            vec3 view = normalize( v_CameraPos - v_WorldPos );\n";
            out << std::format( "            vec3 shaded = ShadeGraphSurface( v_WorldPos, N, view, "
                                "albedo.rgb, {}, {}, {} );\n",
                                metallic, roughness, occlusion );
            out << std::format(
                 "            o_Color = vec4( shaded + ( {} ).rgb, albedo.a * ( {} ) );\n", emission,
                 alpha );
        }
        else
        {
            out << std::format(
                 "            o_Color = vec4( albedo.rgb + ( {} ).rgb, albedo.a * ( {} ) );\n", emission,
                 alpha );
        }
        out << "        }\n    }\n";

        // NO depth-only pass is emitted. There used to be one, named "Depth" and described as the
        // shadow variant, and nothing in the engine ever asked for it: no C++ names "<shader>/Depth",
        // and the shadow pass binds its own pipeline. It could not have served even if something had —
        // it declared no fragment stage at all, while a cascade target is a colour R32F attachment that
        // a shader must WRITE (Shadow.shader writes gl_FragCoord.z).
        //
        // This comment used to add "and its vertex read cameraUB, the camera, rather than the light's
        // matrix". That was WRONG and is corrected rather than deleted, because it invites the wrong
        // repair: Shadow.shader's own vertex reads the same cameraUB and does the same
        // Projection * View * Transform, and MaterialShadow::SetLightMatrix writes the LIGHT's matrices
        // into that block before each cascade draw. Shader text cannot tell a camera from a light here.
        // What was missing was never the matrix — it was any material that would have fed one to this
        // pass. Establishing the same three properties on Unlit.shader is what caught it.
        //
        // Shadow casting for these materials is handled where it belongs, by the engine's shadow
        // pipeline over the generic queue (MeshRenderer::RegisterShadowPass); depth is
        // material-independent, so a per-material depth shader has nothing to contribute.
        out << "}\n";

        return Common::MakeSuccess( out.str() );
    }

    // ---------------------------------------------------------------- migration ---------------
    int MigrateToCatalogue( Document& doc )
    {
        int added = 0;

        // Appends the pins of @p catalogue that @p stored does not have yet, but ONLY while what is
        // stored is a prefix of the catalogue: same names, same types, in the same order. Anything
        // else is a document this function must not touch (see the header).
        const auto grow = [&]( std::vector<Pin>& stored, const std::vector<NodeSpec::PinSpec>& catalogue )
        {
            if ( stored.size() >= catalogue.size() )
                return;
            for ( size_t i = 0; i < stored.size(); ++i )
                if ( stored[i].Name != catalogue[i].Name ||
                     stored[i].Type != static_cast<int>( catalogue[i].Type ) )
                    return;
            for ( size_t i = stored.size(); i < catalogue.size(); ++i )
            {
                stored.push_back( { doc.NextId++, catalogue[i].Name, static_cast<int>( catalogue[i].Type ) } );
                ++added;
            }
        };

        for ( auto& node : doc.Nodes )
        {
            const NodeSpec* spec = FindSpec( node.Kind );
            if ( !spec )
                continue; // unknown kind: ValidateGraph names it; inventing pins for it would not help
            grow( node.Inputs, spec->Inputs );
            grow( node.Outputs, spec->Outputs );
        }
        return added;
    }

    // ---------------------------------------------------------------- serialization -----------
    Common::ResultStr<Loaded> Deserialize( const std::string& json )
    {
        // The BYTES are the engine's business and the CATALOGUE is this file's; the two steps are named
        // separately because only the second one can change what the artist authored.
        auto parsed = ::Desert::Assets::Serialization::ShaderGraph::ParseShaderGraph( json );
        if ( !parsed )
            return Common::MakeError<Loaded>( parsed.GetError() );

        Loaded loaded{ parsed.ExtractValue(), 0 };
        loaded.MigratedPins = MigrateToCatalogue( loaded.Doc );
        return Common::MakeSuccess( std::move( loaded ) );
    }
} // namespace Desert::Editor::ShaderGraph
