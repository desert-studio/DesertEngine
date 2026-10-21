#pragma once

#include <Engine/Assets/TextureAsset.hpp>

namespace Desert::Assets::Mapper
{
    inline const std::string GetTextureType( TextureAsset::Type type )
    {
        switch ( type )
        {
            case Desert::Assets::TextureAsset::Type::Albedo:
                return "AlbedoTexture";
            case Desert::Assets::TextureAsset::Type::Normal:
                break;
            case Desert::Assets::TextureAsset::Type::Metallic:
                break;
            case Desert::Assets::TextureAsset::Type::Roughness:
                break;
            case Desert::Assets::TextureAsset::Type::AO:
                break;
            case Desert::Assets::TextureAsset::Type::Emissive:
                break;
            default:
                break;
        }

        return "UNDEFINED";
    }

} // namespace Desert::Assets::Mapper