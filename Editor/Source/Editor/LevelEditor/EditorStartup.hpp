#pragma once

// THE EDITOR'S START (UE: FEditorLoadingScreen + the EditorInit staging). The staged boot that runs one heavy
// stage per frame behind the splash, the splash's progress weighed in work, the content settle the first scene
// asks for, the thumbnails the hand-over waits on, and the reveal itself: the moment the hidden main window is
// shown and the splash crossfades out.
//
// A member of EditorLayer BY VALUE, built in the layer's constructor because the splash arrives there. Every
// collaborator arrives by reference; it knows nothing of EditorLayer. The layer asks it whether the start is
// still running (StartupLoading, ContentSettling) and calls it at the frame's fixed points.

#include "Editor/Splash/RevealGate.hpp"
#include "Editor/Splash/SplashProgress.hpp"
#include "Editor/Splash/SplashScreen.hpp"

#include <Engine/Assets/ContentGate.hpp>
#include <Engine/Assets/ItemProgress.hpp>
#include <Engine/Core/BootTimeline.hpp>
#include <Common/Core/ResultStr.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Animation
{
    class AnimationLibrary;
}

namespace Desert::Engine
{
    class Application;
}

namespace Desert::Editor
{
    class AssetCompiling;
    class AssetThumbnailPool;
    class ImportManager;
    class SceneFiles;
    class SceneWorkspace;
    class ShotDirector;

    class EditorStartup
    {
    public:
        // @p splash is the start-up splash CreateApplication put up before the renderer existed; this reports its
        // steps to it and takes it down on the first real frame (RevealWhenReady). The stages are planned here;
        // the objects they fill (@p importManager, @p animationLibrary) are created by the layer before the first
        // runs.
        EditorStartup( Engine::Application* application, std::shared_ptr<Assets::AssetManager>& assetManager,
                       std::unique_ptr<ImportManager>&               importManager,
                       std::unique_ptr<Animation::AnimationLibrary>& animationLibrary, SceneWorkspace& workspace,
                       SceneFiles& sceneFiles, AssetCompiling& assetCompiling, const bool& realFrameDrawn,
                       std::unique_ptr<Splash::SplashScreen> splash );

        // The editor's thumbnail pool the hand-over's pictures go through (UE: the editor's
        // FAssetThumbnailPool, which the Content Browser only draws from); null until OnAttach builds it and
        // after OnDetach releases it.
        void AttachThumbnailPool( AssetThumbnailPool* thumbnailPool )
        {
            m_ThumbnailPool = thumbnailPool;
        }

        [[nodiscard]] bool StartupLoading() const
        {
            return m_StartupNext < m_StartupStages.size();
        }
        [[nodiscard]] bool ContentSettling() const
        {
            return m_Content.Loading();
        }
        [[nodiscard]] bool Revealed() const
        {
            return m_Revealed;
        }
        // The splash still covers the editor (a capture records nothing under it).
        [[nodiscard]] bool SplashOnScreen() const
        {
            return m_Splash != nullptr && !m_Revealed;
        }
        // The splash's close button was pressed.
        [[nodiscard]] bool CloseRequested() const
        {
            return m_Splash && m_Splash->CloseRequested();
        }

        // OnAttach: the splash plan, made once the cooked registry is read, and the engine shader compile's stage
        // begun — the longest single wait of the start, and one call (not a stage: the render systems resolve
        // their shaders in their constructors).
        // THE FIRST LEVEL (UE: UEditorEngine::InitEditor's EditorStartupMap). Screenshot mode names its own scene;
        // a project queues its DefaultScene (missing on disk = an error naming it, never a scene built in code);
        // with nothing queued the Basic level template opens as an untitled scene. A capture whose scene is
        // refused closes the application with its status.
        void ChooseInitialLevel( ShotDirector& shots );
        // THE CONTENT THE FIRST FRAME NEEDS (UE: FLevelEditorModule::StartupModule): the cooked asset registry,
        // the engine shaders and their import templates, then the primary scene's systems. A refusal ends the run.
        [[nodiscard]] Common::BoolResultStr BootContent();
        void BeginShaderStage();
        // The item line of the stage running now, for an engine call that works through a list.
        Assets::ItemProgress SplashItems();

        // Staged startup loading: runs ONE heavy stage per frame. True when this frame belongs to the start (a
        // stage ran; the scene is not rendered); false once the stages are done or the splash was closed.
        [[nodiscard]] bool RunStartupFrame();

        /// A scene has just loaded; whatever it asks for has not been asked for yet. Starts the wait.
        void BeginContentSettle();
        /// One tick of the wait: decides whether the frame just rendered closed the chain.
        void UpdateContentSettling();

        // The editor's one thumbnail pump, gated by the reveal (Editor/Splash/RevealGate.hpp).
        void TickThumbnails();

        // Called at every presented frame; the first one presented after the start is over shows the
        // hidden main window and closes the splash. Until then the splash is the only window.
        void RevealWhenReady();

    private:
        // Staged startup loading: the heavy boot work (mesh cooking, asset preload) runs one stage per
        // frame from OnUpdate, each announced on the splash, with the main window still hidden.
        struct StartupStage
        {
            std::string           Label;
            std::function<void()> Run;
            // What one item of this stage costs on the splash's bar, in measured seconds per item, and how
            // many items there are — asked when the plan is made, before any stage runs. Empty = one item.
            double                       SecondsPerItem = 0.01;
            std::function<std::size_t()> CountItems;
            // Instead of CountItems, for a stage whose items do not cost alike: each item's own cost, in
            // the order the stage works through them.
            std::function<std::vector<double>()> ItemCosts;
            std::size_t                          ProgressStage = 0; // its id in m_Progress
        };

        // ===== The splash's progress, and the moment the editor is shown =====
        //
        // THE BAR IS WEIGHED IN WORK (Splash/SplashProgress.hpp): the engine shader compile (OnAttach — not a
        // stage, because the render systems resolve their shaders in their constructors), every entry of
        // m_StartupStages and the settle wait after them, each weighted by its item count times a measured
        // cost per item. The plan is made once the cooked registry is read, which is what counts the items.
        void MakeSplashPlan();
        void BeginSplashStage( std::size_t stage, std::optional<std::size_t> items = std::nullopt );
        void PushSplash();
        // Every condition the splash hand-over depends on, for Splash::MayReveal.
        [[nodiscard]] Splash::RevealState CurrentRevealState() const;
        // THUMB2: before the hand-over, upload the opening folder's cached thumbnails as workers finish
        // them, and hold the hand-over until they are all up (no time bound, THM1n).
        void UploadSplashThumbnails();
        // THUMB3: the open scene's materials — their cached pictures decoded, the missing ones captured on the
        // splash (Splash::SceneThumbnailCaptureAllowed) within Splash::kSceneCaptureBudgetMs.
        void WarmSplashScene();

        Engine::Application*                          m_Application;
        std::shared_ptr<Assets::AssetManager>&        m_AssetManager;
        std::unique_ptr<ImportManager>&               m_ImportManager;
        std::unique_ptr<Animation::AnimationLibrary>& m_AnimationLibrary;
        SceneWorkspace&                               m_Workspace;
        SceneFiles&                                   m_SceneFiles;
        AssetCompiling&                               m_AssetCompiling;
        const bool&                                   m_RealFrameDrawn;
        AssetThumbnailPool*                           m_ThumbnailPool = nullptr; // non-owning (EditorLayer owns it)

        std::vector<StartupStage> m_StartupStages;
        size_t                    m_StartupNext = 0;

        Splash::ProgressModel                 m_Progress;
        std::chrono::steady_clock::time_point m_ProgressEpoch = std::chrono::steady_clock::now();
        std::size_t                           m_ShaderStage   = 0;
        std::size_t                           m_SettleStage   = 0;
        // Scene loads already finished when the settle began: the settle counts only the rest.
        std::size_t m_SettleBase           = 0;
        bool        m_ThumbnailsHoldReveal = false;
        bool        m_SplashWarmStarted    = false;
        std::size_t m_SplashWarmTotal      = 0;     // captures queued when the warm-up started
        std::size_t m_SplashWarmShown      = 0;     // what the splash line last said was left
        bool m_SplashPicturesReasked       = false; // the captures landed and their PNGs were asked for (THM1n-13)
        // When every other reveal condition first held: the start of the thumbnails' budget.
        std::optional<std::chrono::steady_clock::time_point> m_RevealOtherwiseReadySince;
        // KEPT after it is closed, until the layer goes: Close() only starts the crossfade, and the
        // object's destructor is what waits for its window and thread — at teardown, not on the frame
        // the editor has just appeared on.
        std::unique_ptr<Splash::SplashScreen> m_Splash;
        bool                                  m_Revealed = false;
        // The pending count the splash last showed during the settle, so the label is pushed on change only.
        size_t m_SplashOutstandingShown = SIZE_MAX;
        // The splash's close was acted on (Application::Close asked once, not every frame until it lands).
        bool m_QuitFromSplash = false;
        // WHERE THE ELAPSED TOTAL LIVES: `Core::BootTimeline` (sum of the stages, NOT wall clock between the first
        // and the last, because a stage runs one per frame), shared with the shipping runtime so the two boots'
        // numbers mean the same thing. The per-frame scheduler is this class's own.
        ::Desert::Core::BootTimeline m_Boot{ "Editor" };

        // ===== Demand-driven content: the wait that replaced the eager preload =====
        //
        // The cloud kinds are not read at boot; they are read when the scene that wants them says so, on
        // `JobSystem` workers. A sky that appears several frames after the rest of the world is the hitch
        // GAP_ANALYSIS §3.1 warns the lazy model moves into the frame, so the splash that was already up for the
        // staged boot stays up until the content the scene asked for has settled. The rule itself is
        // Engine/Assets/ContentGate.hpp (one implementation, both hosts).
        //
        // `Ready` at construction is correct FOR THIS HOST only: the editor opens on an empty scene
        // behind its own staged-boot overlay, and the first scene load calls BeginWorld. The runtime
        // constructs its gate `Loading`.
        Assets::ContentGate m_Content{ Assets::ContentState::Ready };
    };
} // namespace Desert::Editor
