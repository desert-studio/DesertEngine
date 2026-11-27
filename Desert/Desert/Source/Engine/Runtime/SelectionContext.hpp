#pragma once

#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace Desert::Runtime
{
    // Engine-side "which entities should be visually highlighted/outlined" hint. The ENGINE owns and reads
    // this (e.g. MeshECSSystem flags meshes for the JumpFlood selection outline); higher layers — the
    // editor's SelectionManager — PUSH into it. This keeps the engine free of any editor dependency
    // (modular isolation): the dependency points editor -> engine, never the reverse.
    //
    // Holds the full multi-selection; Get() returns the PRIMARY (most recently selected) entity for
    // consumers that only care about one.
    class SelectionContext final
    {
    public:
        static void Set( const Common::UUID& uuid );

        static void SetAll( std::vector<Common::UUID> uuids );

        // The primary selection (last selected), or nullopt when nothing is selected.
        static std::optional<Common::UUID> Get();

        static const std::vector<Common::UUID>& GetAll();

        static bool Contains( const Common::UUID& uuid );

        static void Clear();

        // Bumped on every selection write: a consumer that derives per-entity state from the selection
        // (SelectionOutlineMask) re-resolves only when it moved, never per frame.
        static uint64_t GetRevision();

        // "Render this entity in its BIND pose, ignoring any animator." Pushed by the editor while Skeleton
        // Edit is active so bone-gizmo edits to the rest pose are actually visible — an auto-playing clip
        // would otherwise override them (MeshECSSystem prefers the animator pose). nullopt = normal render.
        static void SetBindPosePreview( const std::optional<Common::UUID>& uuid );
        static bool IsBindPosePreview( const Common::UUID& uuid );

    private:
        static void Changed();

        static std::vector<Common::UUID> s_Selected;
        static std::unordered_set<Common::UUID> s_SelectedSet; // Contains() in O(1); mirrors s_Selected
        static uint64_t s_Revision;
        static std::optional<Common::UUID> s_BindPosePreview;
    };
} // namespace Desert::Runtime
