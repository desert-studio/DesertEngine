#include <Engine/Assets/AssetManager.hpp>

namespace Desert::Assets
{
    std::vector<AssetManager*>& AssetManager::LiveManagerList()
    {
        static std::vector<AssetManager*> managers;
        return managers;
    }
} // namespace Desert::Assets
