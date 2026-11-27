#include <Engine/Assets/AssetRootPin.hpp>

namespace Desert::Assets
{
    std::mutex& AssetRootPin::Mutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    uint64_t& AssetRootPin::NextId()
    {
        static uint64_t next = 0;
        return next;
    }

    std::unordered_map<uint64_t, std::pair<Common::AssetHandle, std::string>>& AssetRootPin::Pins()
    {
        static std::unordered_map<uint64_t, std::pair<Common::AssetHandle, std::string>> pins;
        return pins;
    }
} // namespace Desert::Assets
