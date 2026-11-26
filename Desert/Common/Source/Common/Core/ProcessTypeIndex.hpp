#pragma once

#include <cstdint>
#include <string_view>

namespace Common
{
    using ProcessTypeId = std::uint32_t;

    // The sequential index of a type in this process, by the type's name: the first name asked gets 0, the next new
    // name 1, and the same name the same index from every DLL (Common is one module). Thread-safe. Used by
    // Common/Core/Entt.hpp for entt::type_index.
    ProcessTypeId ProcessTypeIndex(std::string_view typeName) noexcept;
} // namespace Common
