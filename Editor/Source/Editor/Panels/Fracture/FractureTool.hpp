#pragma once

#include <Engine/Destruction/FractureEdit.hpp>

#include <entt/entt.hpp>
#include <memory>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Desert::Editor
{
    /// The Generate methods by FractureMethod value, as the panel's combo and the palette name them.
    inline constexpr std::array<const char*, 4> kFractureMethodNames = { "Uniform Voronoi", "Clustered Voronoi",
                                                                         "Planar", "Brick" };

    /// THE FRACTURE MODE'S LEVEL PREVIEW (DST-06): with a @p fracture (the mode open and a file read) the
    /// @p selected entity carries ECS::FracturePreviewComponent { fracture, view } and no other entity does; with
    /// none (the mode closed, nothing read, nothing selected) no entity does. Straight on the registry, never
    /// through CommandHistory: preview state is not an edit, so undo never records it, and the component is not
    /// reflected, so a save never writes it (FracturePieces.TheExplodeOffsetIsPreviewStateOnly).
    void SyncFracturePreview( entt::registry& registry, entt::entity selected,
                              const std::shared_ptr<const Destruction::FractureData>& fracture,
                              const Destruction::FractureViewSettings&                view );

    /**
     * @brief THE FRACTURE MODE'S STATE AND ACTIONS (DST-02), below its panel: UE's Fracture Mode keeps the tool
     *        settings in UFractureSettings and acts through its tools; the toolkit only draws them. Ours: one
     *        FractureTool the panel draws and the command palette drives (FractureCommands.hpp), so a frame of
     *        the mode can be made unattended through the editor control socket.
     *
     * The panel shows the file, not a copy that can go stale: Refresh() re-reads the target `.dfrac` when an undo
     * step ran (CommandHistory's revision moved) or the file changed on disk, and adopts it when its bytes differ
     * from what was read - so Ctrl+Z / Ctrl+Y over a Generate show the restored collection on the next frame.
     */
    class FractureTool
    {
    public:
        FractureTool();

        /// The editor's one Fracture mode (the panel and the palette share it).
        static FractureTool& Get();

        /// The target `.dfrac`, relative to the project's Assets folder (an absolute path is taken as is).
        std::string Path = "Fractures/Fracture.dfrac";
        /// The settings the next Generate bakes with; Load takes them from the file.
        Destruction::FractureSettings Settings;
        /// Preview only (UE UFractureSettings::ExplodeAmount / FractureLevel), never written to the asset.
        Destruction::FractureViewSettings View;

        [[nodiscard]] std::filesystem::path File() const;
        /// The fracture as the file holds it (empty until a file is read).
        [[nodiscard]] const Destruction::FractureData& Fracture() const
        {
            return m_Fracture;
        }
        /// The same fracture as one immutable shared object, re-made only when a read changes it (Adopt): the
        /// level preview hands it to the piece draw, whose meshes are cached per FractureData, so a preview
        /// builds its piece meshes once per read, not once per frame. Null until a file is read.
        [[nodiscard]] const std::shared_ptr<const Destruction::FractureData>& SharedFracture() const
        {
            return m_Shared;
        }
        [[nodiscard]] bool Loaded() const
        {
            return m_Loaded;
        }
        /// The last refusal or note, for the panel; empty when the last action succeeded.
        [[nodiscard]] const std::string& Status() const
        {
            return m_Status;
        }

        /// Reads File(): the fracture, its settings; a missing file empties the view (Generate creates it).
        void Load();
        /// Re-reads the file last read when an undo step ran or the file changed; true when it was adopted.
        bool Refresh();

        /// Generate from @p source, the static mesh asset @p sourceMesh: one undo step, then the file is read.
        Common::BoolResultStr Generate( const Geometry::DynamicMesh3&     source,
                                        const Common::Content::AssetGuid& sourceMesh );
        /// UE's Internal Material: the material asset the interior faces draw with (null = none). One undo step.
        Common::BoolResultStr SetInteriorMaterial( const Common::Content::AssetGuid& material );

    private:
        /// What the file looked like when it was last observed; a change triggers a re-read.
        struct FileStamp
        {
            bool                            Exists = false;
            std::uintmax_t                  Size   = 0;
            std::filesystem::file_time_type Written{};

            bool operator==( const FileStamp& ) const = default;
        };
        static FileStamp StampOf( const std::filesystem::path& file );

        /// Load(), keeping the edited Settings: what Generate and SetInteriorMaterial start from.
        void                  ReadKeepingSettings();
        Common::BoolResultStr Commit( const Destruction::FractureData& next, const char* label );
        Common::BoolResultStr Refuse( std::string why );
        /// Adopts @p bytes (empty: no file) as what @p file holds.
        void Adopt( const std::filesystem::path& file, const std::vector<unsigned char>& bytes );

        Destruction::FractureData m_Fracture;
        bool                      m_Loaded = false;
        std::shared_ptr<const Destruction::FractureData> m_Shared; // m_Fracture as last adopted
        std::string               m_Status;

        // What Refresh compares: the file read last, its bytes, its stamp and the undo revision then.
        std::filesystem::path      m_ReadFile;
        std::vector<unsigned char> m_ReadBytes;
        FileStamp                  m_ReadStamp;
        uint64_t                   m_ReadRevision = 0;
    };
} // namespace Desert::Editor
