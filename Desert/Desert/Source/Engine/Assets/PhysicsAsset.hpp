#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Physics/PhysicsAssetFormat.hpp>

namespace Desert::Assets
{
    /**
     * @brief A physics asset on disk (`.dephysasset`), in the engine's ONE asset system — UE's UPhysicsAsset.
     *
     * The same `AssetBase` / `AssetManager` as a mesh or a cloud volume, so it gets the Content Browser, the
     * path-stable reference and the hot reload without a second asset system. The bytes are
     * Engine/Physics/PhysicsAssetFormat.hpp's; this class only reads them through the VFS and writes them.
     *
     * WHAT IT HOLDS. The decoded bodies and joints and the GUID of the skeleton they name. The ragdoll (RAG1b)
     * resolves that skeleton and builds its Jolt bodies through Physics::BuildRagdollDesc; nothing physics lives
     * here (the layer rule).
     */
    class PhysicsAsset final : public AssetBase
    {
    public:
        PhysicsAsset( const Common::Filepath& filepath );

        /// A missing, truncated or foreign-version file is an ERROR naming the reason — never an empty
        /// asset, which would build a ragdoll of nothing.
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Physics::PhysicsAssetData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::PhysicsAsset;
        }

        /// Writes @p asset to @p filepath, creating the directory. A save over an existing `.dephysasset`
        /// keeps that file's GUID (every reference survives); a new file mints one. The caller's GUID is
        /// never copied to a different path — two files would share one identity.
        static Common::BoolResultStr Save( const Common::Filepath&          filepath,
                                           const Physics::PhysicsAssetData& asset );

        /// The GUID the `.dephysasset` envelope header at @p filepath states (VFS first); null when the file is
        /// absent or not a physics asset. The constructor adopts it.
        static Common::Content::AssetGuid ReadPhysicsAssetGuid( const Common::Filepath& filepath );

    private:
        Physics::PhysicsAssetData m_Data;
        bool                      m_Ready = false;
    };
} // namespace Desert::Assets
