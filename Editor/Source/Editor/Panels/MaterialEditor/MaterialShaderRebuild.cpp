#include "MaterialShaderRebuild.hpp"

namespace Desert::Editor
{
    auto MaterialShaderRebuild::Counts() -> std::unordered_map<std::string, uint64_t>&
    {
        static std::unordered_map<std::string, uint64_t> s_Counts;
        return s_Counts;
    }
}
