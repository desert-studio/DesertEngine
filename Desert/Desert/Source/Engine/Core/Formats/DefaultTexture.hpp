#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Desert::Core::Formats
{
    /**
     * @brief What a Texture2D property samples when NOTHING is bound to it.
     *
     * WHY THIS IS AN ENUM AND NOT THE STRING IT USED TO BE. `ShaderParam::DefaultTexture` was a
     * `std::string` the parser filled from `= "white"` and no consumer in the engine ever read (М9 —
     * the parser wrote it, the sampler-binding path never asked). A string is the wrong type for a
     * value with four legal spellings: it can hold "wihte", the parse succeeds, the shader loads, and
     * the misspelling can only ever be discovered by whoever notices the surface is the wrong colour.
     * With a closed set the parser refuses the file and names the alternatives, at the line the typo is
     * on — which is the only moment anyone can act on it.
     *
     * THE IMPLICIT VALUE IS `White`, AND THAT IS NOT A SILENT FALLBACK. A slot with no `= "…"` and a
     * slot written `= "white"` mean the same thing and always did: white is what the backend's own
     * unbound-descriptor fallback already contains (Graphic/API/Vulkan/VulkanFallbackTextures.cpp
     * builds a 1x1 white RGBA8), so this enum's default reproduces the picture the engine drew before
     * the field had a reader rather than quietly choosing a new one.
     *
     * The pixel each name stands for lives in ONE place, `Graphic::DefaultTextures`, and
     * `Desert/Tests/Engine/ShaderSchemaConsumers` asserts that every name here has one there — the two
     * halves of a name->pixel mapping are exactly the "two lists that must agree" shape this codebase
     * keeps paying for.
     *
     * `Kind` IS IN THE NAME because the field that holds one is spelled `DefaultTexture`, and a member
     * whose type shares its name shadows that type for every line after it inside the same class.
     */
    enum class DefaultTextureKind : uint8_t
    {
        White = 0,  ///< 1x1 opaque white. Neutral for anything the shader MULTIPLIES by (albedo, AO, mask).
        Black,      ///< 1x1 opaque black. Neutral for anything the shader ADDS (emissive, a detail overlay).
        Gray,       ///< 1x1 mid grey (128,128,128). Neutral for a signed detail map read around 0.5.
        FlatNormal, ///< 1x1 (128,128,255): tangent-space +Z, i.e. "this surface has no normal detail".
    };

    /// The DSL spelling of @p kind — the token a `.shader` writes and a diagnostic must print back.
    constexpr const char* DefaultTextureKindName( DefaultTextureKind kind )
    {
        // No `default:` on purpose: adding a name to the enum is then a -Wswitch warning in a
        // warning-clean build, instead of a value that silently prints as another one's spelling.
        switch ( kind )
        {
            case DefaultTextureKind::White:
                return "white";
            case DefaultTextureKind::Black:
                return "black";
            case DefaultTextureKind::Gray:
                return "gray";
            case DefaultTextureKind::FlatNormal:
                return "normal";
        }
        return "white";
    }

    /// Every kind, in enum order. The parser's diagnostic, the Graphic pixel table and the census all
    /// read THIS, so none of them can fall behind the enum.
    inline constexpr DefaultTextureKind kAllDefaultTextureKinds[] = {
         DefaultTextureKind::White, DefaultTextureKind::Black, DefaultTextureKind::Gray,
         DefaultTextureKind::FlatNormal };

    /// @return the kind @p name stands for, or nullopt when it stands for nothing — which the parser
    ///         turns into a refusal naming the legal set, never into a quiet White.
    constexpr std::optional<DefaultTextureKind> ParseDefaultTextureKind( std::string_view name )
    {
        for ( const DefaultTextureKind kind : kAllDefaultTextureKinds )
        {
            if ( name == DefaultTextureKindName( kind ) )
                return kind;
        }
        return std::nullopt;
    }
} // namespace Desert::Core::Formats
