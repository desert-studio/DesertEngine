#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <atomic>
#include <cstdint>
#include <memory>

namespace Desert::Assets
{
    /**
     * @brief A VFX system on disk (`.dfx`) — UE's UNiagaraSystem with its emitters embedded (VFX-02).
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a reference survives
     * renames and is the same on every machine. The asset is data only — compiling the stack and simulating
     * belong to VFXWorld (VFX-01/04).
     *
     * EDITED IN PLACE (VFX-PRESETS2, owner 10-13 "variant B"; UE: a dirty UNiagaraSystem package). The `.dfx`
     * window edits THIS asset: Edit() replaces the data with the edited system, so every VFXComponent naming the
     * asset - the window's preview and every level instance - plays the edit (VFXSystemService follows
     * EditSerial). The file is untouched until SaveEdits(); RevertEdits() is LoadFromFile(). The data is held as
     * an immutable snapshot: an edit is a NEW pointer, which is how the effects world sees it changed, and a
     * snapshot a consumer still holds stays valid after the asset moved on or was unloaded.
     */
    class VFXSystemAsset final : public AssetBase
    {
    public:
        VFXSystemAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Common::Content::AssetGuid& Guid() const
        {
            return m_Guid;
        }

        [[nodiscard]] const Serialization::VFXSystemData& GetData() const
        {
            return *m_Data;
        }

        /// The current data as the immutable snapshot consumers keep (never null).
        [[nodiscard]] const std::shared_ptr<const Serialization::VFXSystemData>& Snapshot() const
        {
            return m_Data;
        }

        /// The in-place edit: @p data becomes what the asset holds (a new snapshot) and EditSerial moves. The
        /// caller validated it (ValidateVFXSystemData); the file is not written.
        void Edit( Serialization::VFXSystemData data );

        /// Does the held data differ from what the file held when it was last read or written?
        [[nodiscard]] bool HasUnsavedEdits() const;

        /// Writes the held data to the asset's file; afterwards HasUnsavedEdits is false.
        Common::BoolResultStr SaveEdits();

        /// A file re-read cannot rebuild an asset holding unsaved edits: the edit is the only copy (UE: a dirty
        /// package is not reloaded from disk behind the user's back). Unload refuses for the same reason.
        [[nodiscard]] bool IsReloadableFromFile() const override
        {
            return !HasUnsavedEdits();
        }

        /// THE FILE CHANGED ON DISK WHILE THE ASSET HELD UNSAVED EDITS (UE: the "file changed on disk" question
        /// on a dirty package). Hot reload does not re-read such an asset; it raises this instead, and the
        /// `.dfx` window asks Reload (LoadFromFile: the file wins) or Keep (KeepEdits: the edits win, the next
        /// Save overwrites the file). A read of the file or a save clears it.
        void NoteChangedOnDisk()
        {
            m_ChangedOnDisk = true;
        }
        [[nodiscard]] bool ChangedOnDiskWhileEdited() const
        {
            return m_ChangedOnDisk && HasUnsavedEdits();
        }
        void KeepEdits()
        {
            m_ChangedOnDisk = false;
        }

        /// Moves whenever any VFX system asset's data is replaced (an Edit, a read from the file): the one
        /// counter VFXSystemService compares before looking for a new snapshot.
        [[nodiscard]] static uint64_t EditSerial()
        {
            return s_EditSerial.load( std::memory_order_acquire );
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::VFXSystem;
        }

        /// Writes a VFX system to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&             filepath,
                                           const Serialization::VFXSystemData& data );

    private:
        Common::Content::AssetGuid                           m_Guid;
        std::shared_ptr<const Serialization::VFXSystemData> m_Data =
             std::make_shared<const Serialization::VFXSystemData>();
        // The snapshot the file holds (the last read or write); HasUnsavedEdits compares against it.
        std::shared_ptr<const Serialization::VFXSystemData> m_OnDisk = m_Data;
        bool                                                 m_Ready  = false;
        bool                                                 m_ChangedOnDisk = false;

        static inline std::atomic<uint64_t> s_EditSerial{ 1 };
    };
} // namespace Desert::Assets
