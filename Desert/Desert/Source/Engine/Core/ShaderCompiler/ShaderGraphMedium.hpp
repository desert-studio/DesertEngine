#pragma once

#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Desert::Core
{
    /**
     * WHAT A VOLUME GRAPH OWNS OF ITS OWN: its slice of the reserved binding window, the GLSL names it
     * declares there, and the key its authored values are filed under in a `.demat`.
     *
     * IT IS ONE HEADER BECAUSE ALL THREE ARE HALVES OF PAIRS. The emitter writes
     * `layout( std430, binding = N ) ... CloudMediumParams CloudMedium;` into a Medium block; the
     * renderer calls `SetStorageBuffer( N, ... )` and never consults reflection; the material editor
     * writes a value under a key and the renderer reads it back. Each of those is two files that must
     * spell the same thing, which is the defect shape this project has paid for repeatedly — so the
     * thing is spelled once, here, and both sides include it.
     *
     * THE WINDOW IS SHARED WITH THE SURFACE DOMAIN AND NOT SPLIT FROM IT. A Volume graph becomes a
     * medium compiled into four compute programs; a Surface graph becomes a program of its own. The two
     * are never the same module, so both may start at Core::kGraphOwnedBindingFirst — what the
     * reservation guarantees is that no ENGINE declaration is there, which is measured over every
     * shipped pass by Desert/Tests/Engine/ShaderCacheKey.
     */

    /// The medium's own parameter block. First slot of the window: a medium that exposes anything at all
    /// exposes numbers before it exposes images, so putting the buffer first keeps the texture slots
    /// contiguous and countable from a single base.
    inline constexpr uint32_t kCloudMediumParamsBinding = kGraphOwnedBindingFirst;

    /// The medium's own samplers, one per Texture2D property, in the order the Properties block declares
    /// them.
    inline constexpr uint32_t kCloudMediumTextureFirst = kGraphOwnedBindingFirst + 1u;

    /**
     * HOW MANY IMAGES ONE MEDIUM MAY DECLARE, and it is a bound rather than a preference.
     *
     * Every one of them is a descriptor that FOUR shipped programs have to carry whether the scene has a
     * cloud layer or not — an unbound sampler is an invalid descriptor set, which this backend answers by
     * skipping the dispatch in silence, so each slot costs a fallback binding in four places on every
     * frame of every scene. Four is Unreal's own count for a cloud material's texture parameters and is
     * what the emitter refuses past, by name.
     */
    inline constexpr uint32_t kCloudMediumMaxTextures = 4u;

    /// One past the medium's last possible binding — what a census prints when it says how far the
    /// window has to stay clear.
    inline constexpr uint32_t kCloudMediumBindingEnd = kCloudMediumTextureFirst + kCloudMediumMaxTextures;

    /**
     * HOW MANY NUMERIC PROPERTIES ONE MEDIUM MAY DECLARE, and it is a bound because the buffer that
     * carries them is allocated ONCE, at renderer initialization, before any medium exists.
     *
     * The alternative is reallocating a device buffer whenever the artist adds a node, from inside the
     * frame, where a failure has nowhere to go but a silent skip — the shape this subsystem's whole
     * zero-cost ladder is written to avoid. Sixteen vec4 is 256 bytes per (frame x renderer slot), which
     * is smaller than the hero-cloud instance list already allocated unconditionally beside it, and it
     * is four more parameters than Unreal's cloud material exposes in total.
     */
    inline constexpr uint32_t kCloudMediumMaxValues = 16u;

    /// The parameter buffer's size in bytes: kCloudMediumMaxValues vec4 slots. One spelling, because the
    /// emitter's slot count, the allocation and the upload have to agree and none of them may count.
    inline constexpr uint32_t kCloudMediumParamsBytes = kCloudMediumMaxValues * 4u * 4u;

    // ---- The GLSL the emitter writes and the reflection has to find ------------------------------------

    /// The std430 block's type name, its instance name, and the block name. The instance is what a graph
    /// node's expression reads (`CloudMedium.<Property>`), so it is part of the emitted expression and
    /// not merely cosmetic.
    inline constexpr const char* kCloudMediumStructName   = "CloudMediumParams";
    inline constexpr const char* kCloudMediumBlockName    = "CloudMediumParamsBuffer";
    inline constexpr const char* kCloudMediumInstanceName = "CloudMedium";

    // ---- Where the values live -------------------------------------------------------------------------

    /**
     * THE PREFIX THAT KEEPS TWO SCHEMAS OUT OF EACH OTHER'S NAMES.
     *
     * A cloud material's `.demat` already carries a flat name -> value map, and the SHIPPED cloud schema
     * (CloudRaymarch's Properties) is read out of it by name into the typed Graphic::CloudMaterialValues.
     * A medium's properties are named by whoever drew the graph, so without a separating namespace a graph
     * with a property called `Coverage` would silently retune the layer's bake — one name, two schemas,
     * the value going wherever the reader looked first.
     *
     * A PREFIX RATHER THAN A COLLISION CHECK, deliberately. A check has to be maintained and can only
     * refuse; the prefix makes the collision UNEXPRESSIBLE, because a shipped property name is a GLSL
     * identifier and cannot contain a dot. Graphic::BuildCloudMaterialValues therefore never sees a
     * medium key at all, and the typed struct stays a mirror of one schema rather than a bag of names.
     */
    inline constexpr std::string_view kCloudMediumOverridePrefix = "Medium.";

    /// The `.demat` key a medium property named @p property is authored under.
    inline std::string CloudMediumOverrideKey( std::string_view property )
    {
        return std::string( kCloudMediumOverridePrefix ) + std::string( property );
    }

    /// Whether @p key names a medium property rather than a property of the shipped cloud schema.
    inline bool IsCloudMediumOverrideKey( std::string_view key )
    {
        return key.rfind( kCloudMediumOverridePrefix, 0 ) == 0;
    }
} // namespace Desert::Core
