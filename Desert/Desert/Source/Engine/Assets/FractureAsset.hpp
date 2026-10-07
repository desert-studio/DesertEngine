#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Destruction/FractureFormat.hpp>

namespace Desert::Assets
{
    /**
     * @brief A baked fracture on disk (`.dfrac`), in the engine's ONE asset system — UE's UGeometryCollection.
     *
     * The same `AssetBase` / `AssetManager` as a mesh or a cloud volume, so it gets the Content Browser, the
     * path-stable scene reference and the hot reload without a second asset system. The bytes are
     * Engine/Destruction/FractureFormat.hpp's; this class only reads them through the VFS and writes them.
     *
     * WHAT IT HOLDS. The decoded fracture: every node's piece mesh and hull. The breaking entity (DST-02)
     * builds its pieces and bodies from it; nothing GPU or physics lives here (the layer rule).
     */
    class FractureAsset final : public AssetBase
    {
    public:
        FractureAsset( const Common::Filepath& filepath );

        /// A missing, truncated or foreign-version file is an ERROR naming the reason — never an empty
        /// fracture, which would break into nothing.
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        const Destruction::FractureData& GetFracture() const
        {
            return m_Fracture;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Fracture;
        }

        /// Writes @p fracture to @p filepath, creating the directory. A re-bake over an existing `.dfrac`
        /// keeps that file's GUID (every reference survives); a new file mints one. The caller's GUID is
        /// never copied to a different path — two files would share one identity.
        static Common::BoolResultStr Save( const Common::Filepath&          filepath,
                                           const Destruction::FractureData& fracture );

        /// The GUID the `.dfrac` envelope header at @p filepath states (VFS first); null when the file is
        /// absent or not a fracture. The constructor adopts it.
        static Common::Content::AssetGuid ReadFractureGuid( const Common::Filepath& filepath );

    private:
        Destruction::FractureData m_Fracture;
        bool                      m_Ready = false;
    };
} // namespace Desert::Assets
