#pragma once

#include <Engine/Desert.hpp>
#include <Engine/Assets/AssetManager.hpp>

#include <Common/Core/Constants.hpp>

#include "../IPanel.hpp"

#include <ImGui/imgui.h> // the panel interface no longer hands the toolkit over (IPanel.hpp)
#include "EntityTypeCensus.hpp"

#include <unordered_set>

namespace Desert::Editor
{
    class SceneHierarchyPanel final : public IPanel
    {
    public:
        explicit SceneHierarchyPanel( const std::shared_ptr<Desert::Core::Scene>&  scene,
                                      const std::shared_ptr<Assets::AssetManager>& assetManager )
             : IPanel( "Scene Outliner" ), m_Scene( scene ), m_AssetManager( assetManager )
        {
        }
        void OnUIRender() override;
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

        // The outliner's "Add > Shapes" entry for one primitive: an entity drawing it, one undo step. The
        // palette's "Add shape" entries call the same function, so the two cannot disagree.
        static Common::UUID SpawnPrimitive( Desert::Core::Scene& scene, Geometry::PrimitiveType type );

    private:
        // What the outliner calls this entity — one lookup, so the Type column's text, the row icon's
        // colour and the column's own width all come from the same census (EntityTypeCensus.hpp).
        static EntityTypeKind ClassifyEntity( const ECS::Entity& entity );
        static const char*    GetEntityTypeName( const ECS::Entity& entity );

        void                  DrawEntityNode( ECS::Entity& entity );
        void               DrawInstantiatePrefabPopup();
        void               DrawSavePrefabPopup();
        void               SelectRangeTo( const Common::UUID& target ); // Shift+click

    private:
        std::shared_ptr<Desert::Core::Scene>        m_Scene;
        const std::shared_ptr<Assets::AssetManager> m_AssetManager;
        ImGuiTextFilter                             m_HierarchyFilter;
        ImGuiTextFilter                             m_AddComponentFilter; // shared grouped Add-Component menu
        ImGuiTextFilter                             m_AddEntityFilter;    // grouped "+ Add" (entity) menu
        // Member default -> evaluated at panel construction, i.e. AFTER the project path remap.
        std::string m_PrefabInstantiatePath =
             ( Common::Constants::Path::PREFAB_PATH / "MyPrefab.deprefab" ).string();
        bool m_OpenInstantiatePrefab = false; // deferred OpenPopup

        // WHICH ENTITY THE INSTANCE WILL HANG UNDER, remembered when the popup is opened.
        //
        // The popup is reachable from two places — an entity's context menu and the blank-area menu —
        // and by the time it draws, the entity that opened it is no longer in scope. Before Ю19 that did
        // not matter because the instance always went to the scene root; it matters now, because for a
        // UI prefab the parent decides whether anything is drawn at all, and "Instantiate Prefab" on a
        // canvas that puts the tree beside the canvas is the silent failure this task is about.
        std::optional<Common::UUID> m_InstantiatePrefabParent;

        // "Save as Prefab..." (context menu -> modal at panel scope, same deferred pattern).
        bool                        m_OpenSavePrefab = false;
        std::optional<Common::UUID> m_SavePrefabTarget;
        std::string                 m_SavePrefabPath;

        // Prefab instance tools (Apply overwrites the source file -> confirmation modal; Revert mutates
        // the scene -> deferred like delete).
        bool                        m_OpenApplyPrefab = false;
        std::optional<Common::UUID> m_ApplyPrefabTarget;
        std::optional<Common::UUID> m_PendingPrefabRevert;
        std::optional<Common::UUID> m_PendingPrefabUnpack; // strip the prefab link from a subtree (make local)
        // Deferred structural edits, applied after the UI iteration (they create/destroy entities or edit
        // the Children vectors the tree walk is iterating). Lists: an operation on an entity that is part
        // of the multi-selection applies to the WHOLE selection.
        std::vector<Common::UUID> m_PendingDelete;
        std::vector<Common::UUID> m_PendingDuplicate;

        // {children, newParent} (Null parent = detach to root).
        std::optional<std::pair<std::vector<Common::UUID>, Common::UUID>> m_PendingReparent;

        // Inline rename (double-click the name / context menu -> Rename).
        std::optional<Common::UUID> m_RenamingEntity;
        std::string                 m_RenameBuffer;
        bool                        m_RenameFocusPending = false;

        // Visible draw order of the tree, rebuilt each frame; Shift+click ranges use LAST frame's order
        // (the list the user is actually looking at).
        std::vector<Common::UUID> m_VisibleOrder;
        std::vector<Common::UUID> m_VisibleOrderLast;

        // Reveal-on-select: when the primary selection changes (e.g. a UI element picked in the viewport),
        // force-expand its ancestor chain and scroll it into view so a collapsed/hidden node is revealed.
        Common::UUID                 m_LastRevealedSelection = Common::UUID::Null();
        std::unordered_set<uint64_t> m_ExpandToSelection; // ancestor UUIDs to open once, this frame
        bool                         m_ScrollToSelection = false;
    };
} // namespace Desert::Editor