#pragma once

// THE COMPILE-TIME AXIS OF A SHADER PROGRAM: a set of `#include` targets whose bytes are supplied by
// the caller instead of by the file system, and a set of preprocessor macros defined for the compile.
//
// THE MACROS ARE A PERMUTATION'S ONE SOURCE. A pass that draws the same program into a different target
// (the reflective shadow map draws the G-buffer program, DESERT_GBUFFER_RSM) names its defines HERE and
// nowhere else: ShaderCompiler hands each one to the compile (CompileOptions::AddMacroDefinition) and
// Hash() mixes them into both cache keys, so two permutations are two artifacts. A pipeline built for the
// permutation references the Shader compiled under it (GraphicsPipelineSpecification::Shader); it does
// not carry a define list of its own, which would be a second copy that could disagree with the code.
//
// WHY A VIRTUAL INCLUDE AND NOT A GENERATED PROGRAM. The cloud medium — what a cloud IS at a point in
// space — is sampled by FOUR shipped programs: the view march, the cloud shadow map, the sky occlusion
// volume and the panoramic sky bake that lights the scene (Docs/Clouds/O1_DESIGN.md §10.1, measured by
// zeroing each consumer in turn). Authoring that medium in a material graph by GENERATING a whole
// program would have to be done four times, and the fourth is impossible: BakeProceduralSky is the
// SKY's program — atmosphere and clouds in one panorama — and scenes with no cloud layer need it too,
// so it cannot become a pass of a cloud material. All four instead carry one line that includes the
// medium, and the material substitutes its own text for that one file. The bake gets the right medium
// because it is the SAME program compiled under the same variant, not a second model of clouds beside
// the first.
//
// WHY THE BYTES AND NOT A PATH. A path would make the four programs' text depend on which material is
// being drawn, and shader text is shared. The name stays fixed; only its content moves.
//
// WHAT MAKES THIS SAFE. Two things, and neither is optional:
//
//   * A ShaderVariant is per-COMPILE state. Core::ShaderIncluder is constructed for one compile and
//     dropped when the module is built, so a variant lives exactly as long as the compile that uses it
//     — no global mutable table, and no question about threads.
//   * ComputeShaderCacheKey MIXES the variant. Without that the SPIR-V disk cache would answer a
//     question it was not asked: the same stage, the same file, the same includes on disk, a DIFFERENT
//     medium — and the cached artifact of whichever variant compiled first would be handed to all of
//     them. The failure is invisible (a graph that "does not apply", a sky that is somebody else's)
//     and survives restarts, because the cache is on disk.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    /// One include target whose bytes come from the caller. @p Name is the path as written in the
    /// `#include <...>` line, relative to the shader root, exactly as it would be looked up on disk.
    struct ShaderVirtualSource
    {
        std::string Name;
        std::string Source;
    };

    /**
     * The compile-time variant of a program: the virtual sources it is compiled against and the macros
     * defined for it.
     *
     * Empty is the shipped state and means "every include comes from disk, no macro defined" — the
     * DEFAULT substitution of Generated/CloudMedium.glslh is a real file, so the default variant is not a
     * special case in the includer, in the cache key, or in the shader text.
     */
    struct ShaderVariant
    {
        std::vector<ShaderVirtualSource> VirtualSources;
        /// Macros defined for the compile, each `NAME` or `NAME=VALUE` (ShaderDefineName / ShaderDefineValue).
        std::vector<std::string> Defines;

        [[nodiscard]] bool IsDefault() const
        {
            return VirtualSources.empty() && Defines.empty();
        }

        /// The bytes for @p name, or nullptr when this variant does not override it.
        [[nodiscard]] const std::string* Find( std::string_view name ) const
        {
            for ( const auto& source : VirtualSources )
                if ( source.Name == name )
                    return &source.Source;
            return nullptr;
        }

        /// Content hash of the whole substitution — NAMES AND BODIES BOTH, and order-independent so two
        /// variants that substitute the same files with the same text are one variant however they were
        /// assembled.
        ///
        /// Zero for the default variant, and ComputeShaderCacheKey mixes NOTHING when it is zero — that
        /// is what keeps every artifact already on disk valid, rather than invalidating the whole cache
        /// on the day this axis was added.
        ///
        /// INLINE, and that is not a style choice. The suites that drive the cache key compile a HAND-
        /// PICKED list of engine translation units rather than linking the library, so a definition in a
        /// .cpp is an undefined symbol in each of them until somebody remembers to add the file — a
        /// build break that says nothing about what it is really about. A pure function over the
        /// object's own members has no reason to be anywhere else.
        [[nodiscard]] uint64_t Hash() const
        {
            if ( IsDefault() )
                return 0;

            constexpr uint64_t kFnvOffset = 1469598103934665603ull;
            constexpr uint64_t kFnvPrime  = 1099511628211ull;

            const auto fnv = []( std::string_view data )
            {
                uint64_t h = kFnvOffset;
                for ( unsigned char c : data )
                {
                    h ^= c;
                    h *= kFnvPrime;
                }
                return h;
            };

            // ORDER-INDEPENDENT, by XOR of per-entry hashes: the same substitution assembled in a
            // different order is the SAME variant, or it would compile and cache twice and report a miss
            // for ever. Name and body are hashed TOGETHER, separated by a NUL that cannot occur in a
            // path, so moving text from one virtual include to another is a different variant — two
            // independent hashes XOR-ed would have called those two equal.
            uint64_t hash = 0;
            for ( const auto& source : VirtualSources )
            {
                std::string joined;
                joined.reserve( source.Name.size() + source.Source.size() + 1 );
                joined.append( source.Name );
                joined.push_back( '\0' );
                joined.append( source.Source );
                hash ^= fnv( joined );
            }
            // A define is hashed behind a leading NUL and a tag: no include path starts with a NUL, so a
            // macro can never hash like a virtual source of the same spelling.
            for ( const auto& define : Defines )
            {
                std::string joined;
                joined.reserve( define.size() + 8 );
                joined.push_back( '\0' );
                joined.append( "define" );
                joined.push_back( '\0' );
                joined.append( define );
                hash ^= fnv( joined );
            }

            // ZERO IS THE DEFAULT VARIANT'S VALUE AND NOTHING ELSE MAY WEAR IT. An XOR can land on zero
            // — two entries hashing alike is the obvious way — and a substituting variant that reported
            // zero would read as "no substitution": the cache key would not separate it and every
            // diagnostic that prints it would name the wrong thing. The replacement value is arbitrary;
            // that it is not zero is the point.
            return hash == 0 ? kFnvOffset : hash;
        }
    };

    /// The macro name of a ShaderVariant::Defines entry (`NAME` of `NAME=VALUE`).
    [[nodiscard]] inline std::string_view ShaderDefineName( std::string_view define )
    {
        return define.substr( 0, define.find( '=' ) );
    }

    /// The macro value of a ShaderVariant::Defines entry; empty for a bare `NAME`.
    [[nodiscard]] inline std::string_view ShaderDefineValue( std::string_view define )
    {
        const auto equals = define.find( '=' );
        return equals == std::string_view::npos ? std::string_view() : define.substr( equals + 1 );
    }
} // namespace Desert::Core
