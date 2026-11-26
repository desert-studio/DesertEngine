#include "ProcessTypeIndex.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace Common
{
    ProcessTypeId ProcessTypeIndex(std::string_view typeName) noexcept
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, ProcessTypeId> indices;
        const std::lock_guard lock(mutex);
        return indices.try_emplace(std::string(typeName), static_cast<ProcessTypeId>(indices.size())).first->second;
    }
} // namespace Common
