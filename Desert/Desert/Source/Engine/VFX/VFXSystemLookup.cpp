#include <Engine/VFX/VFXSystemLookup.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/VFXSystemAsset.hpp>

namespace Desert::VFX
{
    VFXSystemLookup MakeAssetSystemLookup( const Assets::AssetManager& assets )
    {
        const Assets::AssetManager* manager = &assets;
        return [manager]( Assets::AssetHandle handle ) -> const Assets::Serialization::VFXSystemData*
        {
            const auto system = manager->FindByHandle<Assets::VFXSystemAsset>( handle );
            return system && system->IsReadyForUse() ? &system->GetData() : nullptr;
        };
    }
} // namespace Desert::VFX
