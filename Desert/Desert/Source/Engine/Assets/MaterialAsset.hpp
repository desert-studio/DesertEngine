#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Graphic/Texture.hpp>

#include <Engine/Assets/AssetManager.hpp>

namespace Desert::Assets
{
    class MaterialAsset : public AssetBase
    {
    public:
        using AssetBase::AssetBase;

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Material;
        }

        virtual Common::UUID GetMaterialUUID() const = 0;

        // The surface template (shader asset) this material draws with, BY HANDLE — its identity.
        // MaterialService routes on it: the engine PBR templates get their C++ material, everything else a
        // generic DataDrivenMaterial. Null = no template resolved (the material draws nothing).
        [[nodiscard]] virtual Common::AssetHandle GetShaderHandle() const = 0;

        // The template's display name (the shader file's stem). Never a key: nothing decides on it.
        virtual std::string GetShaderName() const = 0;
    };

} // namespace Desert::Assets