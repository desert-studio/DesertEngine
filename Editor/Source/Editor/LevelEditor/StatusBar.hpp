#pragma once

// THE LEVEL EDITOR'S STATUS BAR (UE: SStatusBar).
//
// Bottom strip: the bottom drawer's chevron, the background cook, the console line (UE's "Enter Console
// Command"), scene state, selection, entity and triangle counts, documents against the view budget, the snap
// step, then log alerts, unsaved marker, branch/version/config and FPS. A member of EditorLayer BY VALUE; its
// collaborators arrive by reference, and the drawer's chevron — the dock layout's to own — as an action.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace Desert::Editor
{
    class BackgroundCookQueue;
    class DocumentHost;
    class SceneFiles;
    class SceneWorkspace;

    class StatusBar
    {
    public:
        StatusBar( SceneWorkspace& workspace, SceneFiles& sceneFiles, DocumentHost& documents,
                   const std::unique_ptr<BackgroundCookQueue>& backgroundCook,
                   std::function<void()>                       drawBottomDrawerToggle )
             : m_Workspace( workspace ), m_SceneFiles( sceneFiles ), m_Documents( documents ),
               m_BackgroundCook( backgroundCook ), m_DrawBottomDrawerToggle( std::move( drawBottomDrawerToggle ) )
        {
        }

        void Draw();

    private:
        // Triangles drawn by the scene's meshes, summed over entities. Cached — see m_TriangleCache.
        uint64_t SceneTriangleCount();

        SceneWorkspace& m_Workspace;
        SceneFiles&     m_SceneFiles;
        DocumentHost&   m_Documents;
        // Created after the layer attaches, so held through its owner's slot; null while there is none.
        const std::unique_ptr<BackgroundCookQueue>& m_BackgroundCook;
        // The chevron that collapses / restores the bottom drawer (the dock layout owns the node it resizes).
        std::function<void()> m_DrawBottomDrawerToggle;

        // The status bar's console line (UE's "Enter Console Command"); handed to the Lua console to run.
        char m_StatusCmd[256] = {};

        // Triangle census for the status bar. Walking every entity's submeshes each frame is cheap on a
        // 24-entity scene and is not on a large one, so the answer is cached and recomputed on the two
        // things that can change it: an edit (the revision moves) and an entity appearing or vanishing.
        // A mesh finishing an ASYNC load bumps neither, so the cache also has a frame budget — a count
        // that is three seconds stale is a status bar; a count that is permanently wrong is a lie.
        uint64_t m_TriangleCache      = 0;
        uint64_t m_TriangleCacheRev   = static_cast<uint64_t>( -1 );
        size_t   m_TriangleCacheCount = static_cast<size_t>( -1 );
        int      m_TriangleCacheAge   = 0;
    };
} // namespace Desert::Editor
