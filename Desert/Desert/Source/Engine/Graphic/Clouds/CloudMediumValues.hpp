#pragma once

#include <Common/Core/AssetHandle.hpp>

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Core/ShaderCompiler/ShaderGraphMedium.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Graphic
{
    /**
     * @brief The AUTHORED MEDIUM's own parameters and images, resolved from the same `.demat` the shipped
     *        cloud schema is resolved from — and deliberately not through the same struct.
     *
     * WHY THIS IS NOT A FEW MORE FIELDS ON Graphic::CloudMaterialValues. That struct is a TYPED mirror of
     * one shader's Properties block, pinned byte for byte by Desert/Tests/Engine/CloudMaterialSchema so a
     * shipped value cannot acquire a second source of truth. A medium's properties are named by whoever
     * drew the graph — there is no C++ field to mirror them onto, and widening the struct into a name/value
     * bag would destroy exactly the property that census exists to keep. So the two schemas are resolved
     * separately, out of the same flat map, and can never be read as one another:
     * Core::kCloudMediumOverridePrefix is not a legal part of a GLSL identifier, so a shipped property name
     * and a medium key are disjoint by construction rather than by a check somebody has to maintain.
     *
     * WHY A vec4 PER PROPERTY AND NO PACKING RULE. The emitter declares every field of the medium's std430
     * block as a vec4 — which std430 would pad a float to anyway — so this side has nothing to mirror. A
     * padding rule written on both sides of a seam is the shape that turned every cloud in this engine grey
     * once, and it is not worth the twelve bytes it would save per float.
     *
     * ORDER IS THE LAYOUT. Index i of Params is field i of the block, index i of Textures is
     * Core::kCloudMediumTextureFirst + i, and both orders are the order of the medium's Properties block —
     * which the emitter writes and Core::Preprocess::DShaderParser reads back. Neither side counts.
     */
    struct CloudMediumValues
    {
        /// One slot per numeric property, in schema order. Empty when the medium declares none, which is
        /// every shipped scene: no buffer is then created and no descriptor is bound.
        std::vector<glm::vec4> Params;

        /// One handle per Texture2D property, in schema order. A null handle is an authored decision — the
        /// slot is bound to the fallback image, never left unwritten, because an unwritten descriptor makes
        /// the whole set invalid and this backend answers that by skipping the dispatch in silence.
        std::vector<Assets::AssetHandle> Textures;

        bool Empty() const
        {
            return Params.empty() && Textures.empty();
        }
    };

    /**
     * @brief The medium's schema + the material's overrides -> the frame's medium values.
     *
     * @param schema    the medium program's own Properties, in declaration order (empty = no medium, or a
     *                  medium that exposes nothing; both answer an empty result and are the same thing).
     * @param overrides the flattened `.demat` chain — the SAME map BuildCloudMaterialValues reads, filtered
     *                  here by the medium prefix.
     *
     * A property the material does not override keeps the SCHEMA's default, which is the value the graph
     * author typed into the node. That is what makes an unauthored medium and a freshly created material
     * the same picture.
     */
    inline CloudMediumValues BuildCloudMediumValues( const std::vector<Core::Formats::ShaderParam>& schema,
                                                     const MaterialOverrides&                       overrides )
    {
        CloudMediumValues values;
        if ( schema.empty() )
            return values;

        for ( const Core::Formats::ShaderParam& p : schema )
        {
            const std::string key = Core::CloudMediumOverrideKey( p.Name );

            if ( p.IsTexture )
            {
                Assets::AssetHandle handle;
                for ( const auto& [name, raw] : overrides.Textures )
                    if ( name == key )
                        handle = Assets::AssetHandle( raw );
                values.Textures.push_back( handle );
                continue;
            }

            // An asset-reference property is not something a graph can emit (the Volume palette has no node
            // for one) and it carries no GPU slot, so it is skipped rather than given a vec4 nothing reads.
            // Skipping it here is safe for the layout precisely because the emitter cannot produce one:
            // Desert/Tests/Editor/ShaderGraphCompiler asserts the emitted schema is values and textures only.
            if ( p.IsAssetRef() )
                continue;

            // ALL FOUR LANES ARE THE QUESTION HERE, and that is the difference from the cloud material's
            // own reader. A medium's property is not a named field with a known arity: the vec4 is copied
            // into the parameter block whole and the shader decides which lanes it reads, so a NaN in a
            // lane this reader thinks is spare is still a NaN the shader may multiply by. There is no
            // clamp anywhere on this path at all — the argument in Graphic::MaterialValueIsReadable
            // applies a fortiori.
            glm::vec4 value = p.Default;
            for ( const auto& [name, over] : overrides.Params )
                if ( name == key && MaterialValueIsReadable( key, over, MaterialValueLanes::Four ) )
                    value = over;
            values.Params.push_back( value );
        }

        return values;
    }

    /**
     * @brief Everything the authored medium's VALUES can change about the picture, as one comparable
     *        number — the other half of what Graphic::CloudEnvironmentFingerprint already does for the
     *        packed block and for the medium's CODE.
     *
     * IT EXISTS FOR THE SAME REASON THE VARIANT HASH DOES. The medium's code is in the fingerprint because
     * a different medium is a different sky; its VALUES are in it because the same code with a different
     * tint is also a different sky, and the environment bake would otherwise go on lighting the world from
     * the previous one for as long as the sun stood still. Same "middle link drops a property" shape, one
     * link further along.
     *
     * @return 0 exactly when the medium contributes nothing — no properties at all — so "no medium values"
     *         cannot collide with a real set of them.
     */
    inline uint64_t CloudMediumValuesFingerprint( const CloudMediumValues& values )
    {
        if ( values.Empty() )
            return 0ull;

        uint64_t hash = 1469598103934665603ull;

        const auto mix = [&hash]( const void* data, size_t bytes )
        {
            const unsigned char* p = static_cast<const unsigned char*>( data );
            for ( size_t i = 0; i < bytes; ++i )
            {
                hash ^= static_cast<uint64_t>( p[i] );
                hash *= 1099511628211ull;
            }
        };

        for ( const glm::vec4& v : values.Params )
            mix( &v, sizeof( v ) );
        for ( const Assets::AssetHandle& h : values.Textures )
        {
            const uint64_t raw = static_cast<uint64_t>( h );
            mix( &raw, sizeof( raw ) );
        }

        // Never zero: zero is reserved for "this medium has no values", and a collision between that and a
        // real set would leave the world lit by the tint the artist had just changed away from.
        return hash | 1ull;
    }
} // namespace Desert::Graphic
