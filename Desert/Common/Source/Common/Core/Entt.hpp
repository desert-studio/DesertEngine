#pragma once

// EnTT for every engine file: <entt/entt.hpp> with a process-wide type index. Include this header, never
// <entt/entt.hpp> directly.
//
// entt::registry and entt::dispatcher index their pools by entt::type_index<T>::value(), a counter kept in a
// function-local static. Outside Shipping every module is its own DLL, so each DLL would number the types on its
// own and two modules would reach different pools of one registry. Here the index of a type is asked once per DLL
// from Common (ProcessTypeIndex), which numbers type names for the whole process; the name is the specialization's
// own signature, the same text in every DLL of one compiler. The specialization goes through type_index's second
// parameter, the customization point EnTT leaves for it.

#include <Common/Core/ProcessTypeIndex.hpp>

#include <entt/entt.hpp>

#include <type_traits>

static_assert(std::is_same_v<entt::id_type, Common::ProcessTypeId>,
              "Common::ProcessTypeIndex numbers types in entt::id_type");

namespace entt
{
    template <typename Type>
    struct type_index<Type, std::enable_if_t<!std::is_void_v<Type>>>
    {
        static id_type value() noexcept
        {
            static const id_type index = Common::ProcessTypeIndex(ENTT_PRETTY_FUNCTION);
            return index;
        }
    };
} // namespace entt
