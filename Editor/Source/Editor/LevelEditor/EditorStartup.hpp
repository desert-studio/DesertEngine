#pragma once

// THE EDITOR'S START (UE: FEditorLoadingScreen + the EditorInit staging). The staged boot that runs one heavy
// stage per frame behind the splash, its phases reported to the start's progress (Engine/Core/StartupProgress.hpp,
// which the splash samples on its own thread), the content settle the first scene
// asks for, the thumbnails the hand-over waits on, and the reveal itself: the moment the hidden main window is
// shown and the splash crossfades out.
//
// A member of EditorLayer BY VALUE, built in the layer's constructor because the splash arrives there. Every
// collaborator arrives by reference; it knows nothing of EditorLayer. The layer asks it whether the start is
// still running (StartupLoading, ContentSettling) and calls it at the frame's fixed points.

#include "Editor/Splash/RevealGate.hpp"
#include "Editor/Splash/SplashScreen.hpp"

#include <Engine/Assets/ContentGate.hpp>
#include <Engine/Assets/ContentWork.hpp>
#include <Engine/Core/StartupProgress.hpp>
#include <Engine/Core/WorkSink.hpp>
#include <Common/Core/ResultStr.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

    // THE HOST'S PHASES, in start order, after the engine's (Engine::ApplicationStartupPhases). The one list of
    // what the editor's start does; CreateApplication concatenates the two into the progress's plan. The default
    // seconds are the 11-26 warm Bistro start, used only until this machine has a history.
    [[nodiscard]] std::vector<::Desert::Core::StartupPhaseSpec> EditorStartupPhases();

    class EditorStartup
    {
    public:
        // @p splash is the start-up splash CreateApplication put up before the renderer existed; it is taken down
        // on the first real frame (RevealWhenReady). @p startup is the start's progress the host made before the
        // splash (never null): this begins its phases, reports their items and finishes it at the reveal. The
        // stages are planned here; the objects they fill (@p importManager, @p animationLibrary) are created by
        // the layer before the first runs.
        EditorStartup( Engine::Application* application, std::shared_ptr<Assets::AssetManager>& assetManager,
                       std::unique_ptr<ImportManager>&               importManager,
                       std::unique_ptr<Animation::AnimationLibrary>& animationLibrary, SceneWorkspace& workspace,
                       SceneFiles& sceneFiles, AssetCompiling& assetCompiling, const bool& realFrameDrawn,
                       std::unique_ptr<Splash::SplashScreen>            splash,
                       std::shared_ptr<::Desert::Core::StartupProgress> startup );

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
        // The active scene's reflection captures are still being written for the first time
        // (ReflectionCaptureRenderer::FirstFillPending): the reveal and a capture's first frame wait for them.
        [[nodiscard]] bool ReflectionCapturesFilling() const;
        [[nodiscard]] bool Revealed() const
        {
            return m_Revealed;
        }
        // The splash still covers the editor (a capture records nothing under it).
        [[nodiscard]] bool SplashOnScreen() const
        {
            return m_Splash != nullptr && !m_Revealed;
        }
        // THE FIRST LEVEL (UE: UEditorEngine::InitEditor's EditorStartupMap). Screenshot mode names its own scene;
        // a project queues its DefaultScene (missing on disk = an error naming it, never a scene built in code);
        // with nothing queued the Basic level template opens as an untitled scene. A capture whose scene is
        // refused closes the application with its status.
        void ChooseInitialLevel( ShotDirector& shots );
        // THE CONTENT THE FIRST FRAME NEEDS (UE: FLevelEditorModule::StartupModule): the cooked asset registry,
        // the engine shaders and their import templates, then the primary scene's systems. A refusal ends the run.
        // Begins content.gather; the shader compile (one call, not a stage: the render systems resolve their
        // shaders in their constructors) begins the three shader phases itself.
        [[nodiscard]] Common::BoolResultStr BootContent();

        // Staged startup loading: runs ONE heavy stage per frame. True when this frame belongs to the start (a
        // stage ran; the scene is not rendered); false once the stages are done or the splash was closed.
        [[nodiscard]] bool RunStartupFrame();

        /// A scene has just loaded; whatever it asks for has not been asked for yet. Starts the wait.
        /// @p finishedBefore is `AsyncAssetLoader::Progress().Finished` taken before the load asked for anything.
        void BeginContentSettle( uint64_t finishedBefore );
        /// The start's progress while the start's scene opens (the open begins scene.read and scene.init through
        /// it, and names its steps), else null: a scene opened after the start reports nothing.
        [[nodiscard]] ::Desert::Core::WorkSink* SceneLoadSink() const;
        /// What a scene load that BLOCKS reports while it waits for its reads (LOAD-SHOW-b, UE's
        /// FScopedSlowTask shape): installed around the load with Assets::ScopedWaitFeedback, it turns each read
        /// landing inside AsyncAssetLoader::AwaitOne into the settle's line, counted since @p finishedBefore —
        /// the splash draws it on its own thread while the main thread is held.
        [[nodiscard]] Assets::AsyncAssetLoader::WaitFeedback SceneLoadFeedback( uint64_t finishedBefore );
        /// What the settle is reading now and how far it is (Engine/Assets/ContentWork.hpp), for the overlay
        /// the editor draws while a scene opened after the reveal loads.
        [[nodiscard]] const Assets::ContentProgressLine& ContentProgress() const
        {
            return m_ContentProgress;
        }
        /// One tick of the wait: decides whether the frame just rendered closed the chain.
        void UpdateContentSettling();

        // The editor's one thumbnail pump, gated by the reveal (Editor/Splash/RevealGate.hpp).
        void TickThumbnails();

        // Called at every presented frame; the first one presented after the start is over shows the
        // hidden main window and closes the splash. Until then the splash is the only window.
        void RevealWhenReady();

    private:
        // Staged startup loading: the heavy boot work runs one stage per frame from OnUpdate, each one phase of
        // the progress (its key in EditorStartupPhases, which owns the label), with the main window still hidden.
        // The stage reports its own items into the sink it is handed.
        struct StartupStage
        {
            std::string                                      Key;
            std::function<void( ::Desert::Core::WorkSink* )> Run;
        };

        // ===== The start's progress, and the moment the editor is shown =====
        //
        // THE BAR IS THE PROGRESS'S (Engine/Core/StartupProgress.hpp): every phase is weighed by the seconds it
        // took on the previous start on this machine, the phase running now by its measured rate. This class
        // only begins the phases at the points the start reaches them; a key the plan lacks is a defect, logged.
        void BeginPhase( std::string_view key, ::Desert::Core::LiveSource source = {} );
        // The Discovered counts of the scene phases, pulled on the splash thread; bases taken now, on the main
        // thread. scene.read: the loader's reads. scene.settle: the reads plus the engine pipelines.
        [[nodiscard]] static ::Desert::Core::LiveSource SceneReadSource();
        [[nodiscard]] static ::Desert::Core::LiveSource SceneSettleSource();
        // scene.stream: the textures streaming, counted on the main thread into m_Stream.
        [[nodiscard]] ::Desert::Core::LiveSource StreamSource() const;
        // Per presented frame until the reveal: begins scene.stream once the scene's content has settled, and
        // names what the reveal still waits for.
        void ReportRevealWait( const Splash::RevealState& state );
        // At the reveal (or, with no splash, when it would have been): the progress's table, and the history the
        // next start weighs its phases by.
        void FinishStartup();
        // The settle's line (m_ContentProgress), read by the overlay a scene opened after the reveal draws.
        void ShowContentLine( const Assets::ContentProgressLine& line );
        // Every condition the splash hand-over depends on, for Splash::MayReveal.
        [[nodiscard]] Splash::RevealState CurrentRevealState() const;
        // THM-FIXB: the kinds of the project that get no picture, said once when the window is shown.
        static void ReportUnproducedThumbnailKinds();

        Engine::Application*                          m_Application;
        std::shared_ptr<Assets::AssetManager>&        m_AssetManager;
        std::unique_ptr<ImportManager>&               m_ImportManager;
        std::unique_ptr<Animation::AnimationLibrary>& m_AnimationLibrary;
        SceneWorkspace&                               m_Workspace;
        SceneFiles&                                   m_SceneFiles;
        AssetCompiling&                               m_AssetCompiling;
        const bool&                                   m_RealFrameDrawn;
        AssetThumbnailPool* m_ThumbnailPool = nullptr; // non-owning (EditorLayer owns it)

        std::vector<StartupStage> m_StartupStages;
        size_t                    m_StartupNext = 0;

        // The start's progress, shared with the host (its owner) and the splash (its reader).
        std::shared_ptr<::Desert::Core::StartupProgress> m_Startup;
        bool m_StartupFinished = false; // StartupProgress::Finish was called
        // THE SCENE PHASE THIS HOST BEGAN LAST (the open's own scene.read / scene.init are begun by the open
        // through m_SceneWatch): Opening until BeginContentSettle, then Settling, then Streaming.
        enum class StartupScene : uint8_t
        {
            Opening,
            Settling,
            Streaming,
        };
        StartupScene m_StartupScene = StartupScene::Opening;
        // THE OPEN'S SINK (GoF Decorator over m_Startup): forwards the items, and gives scene.read its count
        // source when the open enters that step -- the open names its steps, this host owns their counts.
        class SceneOpenWatch final : public ::Desert::Core::WorkSink
        {
        public:
            explicit SceneOpenWatch( ::Desert::Core::StartupProgress& progress ) : m_Progress( progress ) {}
            void                   BeginStep( std::string_view key ) noexcept override;
            void                   Expect( uint64_t items ) noexcept override;
            [[nodiscard]] uint64_t BeginItem( std::string_view name ) noexcept override;
            void                   FinishItem( uint64_t token ) noexcept override;
            [[nodiscard]] bool     StopRequested() const noexcept override;

        private:
            ::Desert::Core::StartupProgress& m_Progress;
        };
        SceneOpenWatch m_SceneWatch;
        // THE TEXTURES STREAMING, counted on the main thread (TextureService is main-thread) and read by
        // StreamSource on the splash thread; shared so the source outlives nothing it reads.
        struct StreamCount
        {
            std::atomic<uint64_t> Done{ 0 };     // completions seen between frames since scene.stream began
            std::atomic<uint64_t> InFlight{ 0 }; // streaming now
        };
        std::shared_ptr<StreamCount> m_Stream = std::make_shared<StreamCount>();
        uint64_t                     m_StreamLast = 0; // InFlight at the previous frame (main thread)
        // Reads the loader had finished when the scene load began: the settle counts only the rest.
        uint64_t                    m_SettleBase = 0;
        Assets::ContentProgressLine m_ContentProgress; // the line the settle shows now
        // KEPT after it is closed, until the layer goes: Close() only starts the crossfade, and the
        // object's destructor is what waits for its window and thread — at teardown, not on the frame
        // the editor has just appeared on.
        std::unique_ptr<Splash::SplashScreen> m_Splash;
        bool                                  m_Revealed = false;
        // The splash's close was acted on (Application::Close asked once, not every frame until it lands).
        bool m_QuitFromSplash = false;

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
