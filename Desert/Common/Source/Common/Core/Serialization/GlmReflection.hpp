#pragma once

/**
 * THE ONE PLACE `rfl::Reflector` IS SPECIALISED FOR A glm TYPE. Do not add a second.
 *
 * There were two. `Engine/Core/Serialize/GLMReflect.hpp` specialised `Reflector<glm::vec2/3/4>` with the
 * same wire form as this file and WITHOUT `quat` or `mat4`, and thirteen translation units included it
 * instead of this one. A specialisation of the same template for the same type in two headers is not a
 * style question: including both in one translation unit is ill-formed, so the two halves of the tree
 * could never meet. The symptom was not a compile error anybody saw — it was that adding `quat` to a
 * struct served by the other header failed for a reason that read like a missing include, and that the
 * file a developer reached for decided which types they were allowed to serialise.
 *
 * WHY THIS HEADER IS THE SURVIVOR rather than the smaller one: it is the superset (quat and mat4), it
 * lives in `Common`, which everything already links, and the transforms this engine serialises are
 * quaternions — a serialisation header that cannot spell a rotation is the one that has to grow.
 *
 * `Desert/Tests/Engine/GlmReflectorCensus` asserts the RELATION rather than this fix: "a specialisation
 * of `rfl::Reflector` for a glm type exists in exactly one header of this tree", with the count derived
 * from the files it finds. A one-off deletion is how a second source of truth comes back.
 */

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/compatibility.hpp>

#include <Common/Core/UUID.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Math/AABB.hpp>

namespace rfl
{
    template <>
    struct Reflector<Common::Math::AABB>
    {
        struct ReflType
        {
            glm::vec3 Min;
            glm::vec3 Max;
        };

        static inline Common::Math::AABB to( const ReflType& r ) noexcept
        {
            Common::Math::AABB aabb;
            aabb.Min = r.Min;
            aabb.Max = r.Max;
            return aabb;
        }

        static inline ReflType from( const Common::Math::AABB& aabb )
        {
            return ReflType{ aabb.Min, aabb.Max };
        }
    };

    template <>
    struct Reflector<Common::UUID>
    {
        using ReflType = uint64_t;

        static inline Common::UUID to( const ReflType& value ) noexcept
        {
            return Common::UUID{ value };
        }

        static inline ReflType from( const Common::UUID& value )
        {
            return static_cast<uint64_t>( value );
        }
    };

    // AssetHandle is a distinct type from UUID -> needs its own reflector. Same bare-uint64 wire form.
    template <>
    struct Reflector<Common::AssetHandle>
    {
        using ReflType = uint64_t;

        static inline Common::AssetHandle to( const ReflType& value ) noexcept
        {
            return Common::AssetHandle{ value };
        }

        static inline ReflType from( const Common::AssetHandle& value )
        {
            return static_cast<uint64_t>( value );
        }
    };

    template <>
    struct Reflector<glm::mat4>
    {
        using ReflType = std::array<float, 16>;

        static inline glm::mat4 to( const ReflType& arr ) noexcept
        {
            glm::mat4 result( 1.0f );

            // column-major layout
            result[0][0] = arr[0];
            result[0][1] = arr[1];
            result[0][2] = arr[2];
            result[0][3] = arr[3];

            result[1][0] = arr[4];
            result[1][1] = arr[5];
            result[1][2] = arr[6];
            result[1][3] = arr[7];

            result[2][0] = arr[8];
            result[2][1] = arr[9];
            result[2][2] = arr[10];
            result[2][3] = arr[11];

            result[3][0] = arr[12];
            result[3][1] = arr[13];
            result[3][2] = arr[14];
            result[3][3] = arr[15];

            return result;
        }

        static inline ReflType from( const glm::mat4& m )
        {
            return { m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
                     m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3] };
        }
    };

    template <>
    struct rfl::Reflector<glm::vec2>
    {
        using ReflType = std::array<float, 2>;

        static inline glm::vec2 to( const ReflType& arr ) noexcept
        {
            return glm::vec2( arr[0], arr[1] );
        }

        static inline ReflType from( const glm::vec2& v )
        {
            return { v.x, v.y };
        }
    };

    template <>
    struct Reflector<glm::vec3>
    {
        using ReflType = std::array<float, 3>;

        static inline glm::vec3 to( const ReflType& arr ) noexcept
        {
            return glm::vec3( arr[0], arr[1], arr[2] );
        }

        static inline ReflType from( const glm::vec3& v )
        {
            return { v.x, v.y, v.z };
        }
    };

    template <>
    struct Reflector<glm::vec4>
    {
        using ReflType = std::array<float, 4>;

        static inline glm::vec4 to( const ReflType& arr ) noexcept
        {
            return glm::vec4( arr[0], arr[1], arr[2], arr[3] );
        }

        static inline ReflType from( const glm::vec4& v )
        {
            return { v.x, v.y, v.z, v.w };
        }
    };

    template <>
    struct Reflector<glm::quat>
    {
        using ReflType = std::array<float, 4>;

        static inline ReflType from( const glm::quat& v )
        {
            return { v.w, v.x, v.y, v.z };
        }

        static inline glm::quat to( const ReflType& arr ) noexcept
        {
            return glm::quat( arr[0], arr[1], arr[2], arr[3] );
        }
    };
} // namespace rfl