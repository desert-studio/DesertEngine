#pragma once

// WHICH ENTITIES OF A PARTITIONED WORLD A SAVE MUST WRITE (WP17).
//
// The pattern is UE's One File Per Actor dirty flag, and not its letter. In UE every actor's external package
// carries "dirty"; Modify() inside a transaction sets it, and the transaction remembers the package's state so
// undo puts it back; a save writes the dirty packages only. Here an entity's "package" is its `.deent` file
// (ExternalEntities.hpp), and the flag is a REVISION rather than a bit:
//
//   - every recorded edit stamps the entities it changed with a fresh revision (Touch) and keeps the revision
//     each had before; undo puts the old revision back (Restore), redo the new one;
//   - a save (or the open that read the files) takes a BASELINE: the revision every entity had when its file
//     was last made to match it;
//   - an entity is dirty when its revision is not its baseline's.
//
// A bit could not say "undone back to what is on disk" after an intervening save: edit, save, undo leaves a
// bit cleared by the save, yet the entity now differs from its file. Revisions compare equal exactly when the
// entity is the one the file holds, so edit-then-undo is clean and edit-save-undo is dirty, both without
// serializing anything.
//
// WHAT NEEDS NO STAMP. A record states its parent and its sibling index (Assets::EntityData), and those change
// when OTHER entities are created, deleted or reparented. They are cheap to read off the live tree, so the
// baseline keeps them and the plan compares them; a command does not have to know its neighbours.
//
// WHAT FALLS BACK TO THE WHOLE SCENE. An edit recorded without naming its entities (TouchAll), a save to a file
// other than the baseline's, a scene that is not partitioned, and an entity that is gone without being a
// record of its own (a prefab instance's child: the record that stated it cannot be named any more). Each is
// the ordinary whole save - every record serialized, each file rewritten only if its bytes differ.

#include <Engine/Core/Serialize/EditStamps.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Json/Carry.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Core
{
    // One live entity as a save sees it. Cheap to gather: no component is read.
    struct LiveEntity
    {
        Common::UUID Id;
        Common::UUID Record;            // the entity whose record states this one (itself, unless it is inside a
                                        // prefab instance or a foliage field: then the instance's / field's root)
        Common::UUID  Parent;           // null for a root
        std::uint32_t SiblingIndex = 0; // what the record's siblingIndex states (records only)
    };

    // What a save of a partitioned world has to do.
    struct PackageSavePlan
    {
        bool                      Whole = true; // serialize and write every record (the reason is in WholeReason)
        const char*               WholeReason = "no baseline";
        std::vector<Common::UUID> Listed;  // every record id, ascending (the header's list order)
        std::vector<Common::UUID> Changed; // records to serialize and write, ascending
        std::vector<Common::UUID> Removed; // records of the baseline the scene no longer has
        // WP19: records of the world the scene does not hold (an editor region left them on disk), ascending.
        // Listed names them, the header keeps them, and no file of theirs is written, removed or checked.
        std::vector<Common::UUID> NotLoaded;
        // Not empty: the save cannot be made without losing the not-loaded records (a save to another file);
        // nothing is written and this is the reason.
        std::string Refusal;
    };

    class EntityPackages final : public IEditStamps
    {
    public:
        // IEditStamps - what the undo history uses (EditStamps.hpp).
        Stamp Touch( Common::UUID id ) override;
        void  Restore( Common::UUID id, Revision revision ) override;
        void  TouchAll() override;

        // An edit to entity `id` made OUTSIDE the history (a tool's direct write, an editor system, an import's
        // rebind - UE's Modify()/MarkPackageDirty without a transaction): no undo record can put it back, so the
        // entity stays dirty until a save or an open takes the baseline, whatever undo/redo does to its revision.
        void MarkModified( Common::UUID id );
        // Load or Clear: nothing is known about any file.
        void Forget();

        // The files of the scene at `scenePath` now hold every entity of `live` as it is.
        void Baseline( const std::filesystem::path& scenePath, std::span<const LiveEntity> live );

        [[nodiscard]] bool IsDirty( Common::UUID id ) const;

        // The files the baseline was taken from (the scene's open or last save), if any: the world on disk an
        // editor region loads from (WP19, EditorRegions.hpp).
        [[nodiscard]] const std::optional<std::filesystem::path>& BaselinePath() const
        {
            return m_BaselinePath;
        }

        // WP19, UE's editor loader adapter: the scene now holds `live` of the baseline's world, and the records
        // `notLoaded` stay on disk. An entity new to the baseline was just read from its file (clean); one of the
        // baseline that is no longer live was unloaded, NOT deleted - its file is kept and the header lists it.
        // Forget clears the set; a save keeps it.
        void AdoptRegion( std::span<const LiveEntity> live, std::span<const Common::UUID> notLoaded );

        [[nodiscard]] bool IsLoaded( Common::UUID record ) const
        {
            return !m_NotLoaded.contains( static_cast<std::uint64_t>( record ) );
        }
        [[nodiscard]] std::size_t NotLoadedCount() const
        {
            return m_NotLoaded.size();
        }

        // PURE: what a save of the scene to `scenePath`, whose live entities are `live`, must write.
        [[nodiscard]] PackageSavePlan Plan( const std::filesystem::path& scenePath,
                                            std::span<const LiveEntity> live, bool partitioned ) const;

    private:
        struct SavedRecord
        {
            std::uint64_t Parent       = 0;
            std::uint32_t SiblingIndex = 0;
        };

        [[nodiscard]] Revision CurrentOf( std::uint64_t id ) const;

        Revision                                       m_Next = 1;
        std::unordered_map<std::uint64_t, Revision>    m_Current; // absent = 0 (never edited)
        std::unordered_set<std::uint64_t>              m_Unrecorded; // MarkModified since the baseline
        bool                                           m_Whole = false;
        std::optional<std::filesystem::path>           m_BaselinePath;
        std::unordered_map<std::uint64_t, Revision>    m_SavedRevision; // every live entity at the baseline
        std::unordered_map<std::uint64_t, SavedRecord> m_SavedRecords;  // the records among them
        std::unordered_set<std::uint64_t>              m_NotLoaded;     // records left on disk (AdoptRegion)
    };

    // What one save did.
    struct PackageSaveOutcome
    {
        ExternalEntities::WriteOutcome Files;
        std::size_t                    Serialized = 0; // records the composer was asked for and returned
        bool                           Whole      = true;
    };

    // Composes the scene document: every record when `only` is null, else the records whose id is in `only`
    // (and every non-record member). Returns the document; the caller's serializer is the only cost.
    using ComposeScene = std::function<Common::ResultStr<Common::Json::TextDocument>(
         const std::unordered_set<std::uint64_t>* only )>;

    // Whether a delta save proves the entities it skips (ExternalEntities::VerifyCleanRecords).
    enum class CleanCheck : std::uint8_t
    {
        Trust,        // Release: the packages are the truth; only the changed records are composed
        AgainstFiles, // Debug: every record is composed, and each clean one must be its file's bytes
    };

    // The editor's choice: the check costs a serialization and a file read per clean entity.
#if defined( DESERT_CONFIG_DEBUG )
    inline constexpr CleanCheck kEditorCleanCheck = CleanCheck::AgainstFiles;
#else
    inline constexpr CleanCheck kEditorCleanCheck = CleanCheck::Trust;
#endif

    // THE SAVE OF A SCENE FILE THROUGH ITS PACKAGES: plans, composes what the plan names, writes it
    // (ExternalEntities::WriteSceneFile whole, WriteSceneDelta otherwise), and on success takes the baseline.
    // With CleanCheck::AgainstFiles a delta save first refuses - writing nothing - when an entity it would skip
    // differs from its file: an edit that marked nothing.
    [[nodiscard]] Common::ResultStr<PackageSaveOutcome>
    SaveThroughPackages( const std::filesystem::path& scenePath, EntityPackages& packages,
                         std::span<const LiveEntity> live, bool partitioned, const ComposeScene& compose,
                         CleanCheck check );
} // namespace Desert::Core
