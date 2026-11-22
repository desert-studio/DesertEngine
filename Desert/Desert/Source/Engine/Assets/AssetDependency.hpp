#pragma once

#include <Engine/Assets/Common.hpp>

#include <memory>

namespace Desert::Assets
{
    // A typed reference from one asset to another: the handle it stores, and the loaded asset once
    // ResolveDependencies found it. Apart from AssetManager.hpp, so an asset header that holds one does not
    // pull in the manager (UE: TSoftObjectPtr beside UAssetManager); Resolve is given the manager by the .cpp
    // that has it.
    template <typename T>
    struct AssetDependency
    {
        AssetHandle      Handle;
        std::weak_ptr<T> Cached;

        template <typename Manager>
        void Resolve( Manager& manager )
        {
            Cached = manager.template FindByHandle<T>( Handle );
        }

        bool IsValid() const
        {
            return !Cached.expired();
        }

        T* Get() const
        {
            if ( auto ptr = Cached.lock() )
                return ptr.get();
            return nullptr;
        }
    };
} // namespace Desert::Assets
