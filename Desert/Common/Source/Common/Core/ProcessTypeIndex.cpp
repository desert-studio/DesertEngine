#include "ProcessTypeIndex.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace Common
{
    namespace
    {
        // A type inside an anonymous namespace is local to its translation unit: two of them with one spelling are
        // two types (each DLL, each .cpp its own), and the name alone would give them one index and one pool.
        // MSVC spells the namespace `anonymous namespace', Clang (anonymous namespace), GCC {anonymous}.
        bool IsTranslationUnitLocal(std::string_view typeName) noexcept
        {
            return typeName.find("anonymous namespace") != std::string_view::npos ||
                   typeName.find("{anonymous}") != std::string_view::npos;
        }
    } // namespace

    ProcessTypeId ProcessTypeIndex(std::string_view typeName) noexcept
    {
        static struct
        {
            std::mutex                                     mutex;
            std::unordered_map<std::string, ProcessTypeId> byName;
            ProcessTypeId                                  next = 0;
        } state;
        const std::lock_guard lock(state.mutex);
        // A translation-unit-local type is asked once (its entt::type_index is local too) and never matched by name.
        if (IsTranslationUnitLocal(typeName))
            return state.next++;
        const auto [it, added] = state.byName.try_emplace(std::string(typeName), state.next);
        if (added)
            ++state.next;
        return it->second;
    }
} // namespace Common
