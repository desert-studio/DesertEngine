#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Graphic/Texture.hpp>

#include <Engine/Assets/AssetManager.hpp>

#include <Engine/Core/Formats/ImageFormat.hpp>

namespace Desert::Assets
{
    class TextureAsset : public AssetBase
    {
    public:
        // A CLASSIFICATION of material texture slots, not a property of this asset: SurfaceMaterialAsset
        // and Mapper key sampler names and slot maps on it. No TextureAsset instance stores one — the
        // `m_Type` member GetType() used to return was never assigned by anything and read back
        // uninitialized memory, so both were deleted rather than kept looking wired.
        enum class Type
        {
            Albedo,
            Normal,
            Metallic,
            Roughness,
            AO,
            Emissive,
            //******//
            Skybox
        };

        explicit TextureAsset( AssetPriority priority, const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;

        virtual Common::BoolResultStr Unload() override;

        bool IsReadyForUse() const override
        {
            return m_IsReadyForUse;
        }

        // The source image path THIS machine can open — the stored root-tagged key, expanded by Load().
        const auto& GetSourcePath() const
        {
            return m_SourcePath;
        }

        // ONE identity, not two. This used to be a separate `m_Handle` member that Load assigned alongside
        // m_Metadata.Handle, with a comment explaining that the two must be kept in lock-step or every
        // handle-based resolve would miss. Two fields that must agree, and nothing checking that they do,
        // is the defect shape this whole change is about — so there is now one field.
        const auto& GetHandle() const
        {
            return m_Metadata.Handle;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Texture2D;
        }

    private:
        // Load() fills exactly what consumers read. This used to also declare m_Type, m_CookedPath,
        // m_Width/m_Height/m_Channels and m_Format — none of which Load() (or anything else) ever
        // assigned, so every one of them was uninitialized memory wearing a member's name.
        bool        m_IsReadyForUse = false;
        std::string m_SourcePath;
    };

} // namespace Desert::Assets