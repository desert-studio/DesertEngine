#pragma once

#include "MovieRender.hpp"

#include <Engine/UI/Ecs/RegistryUICanvasResources.hpp>
#include <Engine/Core/PlayerStart.hpp>
#include <Common/Core/DevInstruments.hpp>
#include <Engine/Assets/ContentGate.hpp>
#include <Engine/Core/BootTimeline.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Desert.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <UI/UICanvasContext.hpp>
#include <Engine/UI/Ecs/UICanvasRendererEcs.hpp>

#include <entt/entt.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Desert::Graphic
{
    class Framebuffer;
    class RenderPass;
    class GraphicsPipeline;
    class MaterialExecutor;
} // namespace Desert::Graphic
namespace Desert::Media
{
    class MediaAudioOutput;
    class MediaTexture;
    class StartupMoviePlayer;
} // namespace Desert::Media
namespace Desert::Graphic::Render2D
{
    class DrawList2D;
    class Render2D;
    class UIRenderTextureCache;
}

namespace Desert::Player
{
    // The PLAYER layer: loads the opened project's scene, flips it straight into Play (scripts, physics,
    // gameplay camera all live) and blits the rendered frame fullscreen into the swapchain pass, drawing
    // the game's UI over it with the engine's own Render2D batcher. No panels, no gizmos, no editing —
    // the game, exactly as Play-in-editor runs it.
    // (namespace Player: Desert::Runtime already belongs to the engine's runtime services.)
    class RuntimeLayer : public Common::Layer
    {
    public:
        // scenePathOverride: from `--scene <path>`; empty -> the project's DefaultScene.
        // application: the owner, so a UI "quit" button can ask for an ordered close instead of calling
        // std::exit() from inside the frame (which destroys the job system's mutexes under its own live
        // worker threads — see the quit handler in OnUpdate).
        // @p play: how the FIRST level begins Play (`--player-start`); a level switch begins with the default
        // start, since a tag names a start in the level it was given for.
        // @p movie: the --render-movie request; a development build's only (MovieRender.hpp).
#if DESERT_DEV_INSTRUMENTS
        RuntimeLayer( std::string scenePathOverride, Core::PlayRequest play,
                      std::optional<MovieRenderRequest> movie, Engine::Application* application );
#else
        RuntimeLayer( std::string scenePathOverride, Core::PlayRequest play, Engine::Application* application );
#endif
        ~RuntimeLayer();

        [[nodiscard]] Common::BoolResultStr OnAttach() override;
        [[nodiscard]] Common::BoolResultStr OnDetach() override;
        [[nodiscard]] Common::BoolResultStr OnUpdate( const Common::Timestep& ts ) override;
        [[nodiscard]] Common::BoolResultStr OnUIRender() override;
        bool                                OnMouseScrolled( Common::MouseScrolledEvent& scroll );
        bool                                OnKeyTyped( Common::KeyTypedEvent& typed );
        bool                                OnKeyPressed( Common::KeyPressedEvent& key );
        /// The frame is out. It counts presented frames in every build; in a development build it is also
        /// the half of the unattended capture that COLLECTS — see RuntimeShot.hpp.
        void OnFramePresented() override;

    private:
        // Tear down the current scene and deserialize `path` in its place (systems survive Clear()). The load
        // half of Core::LevelTravel -- run only from its TickTravel at the head of OnUpdate, between frames.
        // A refusal before teardown leaves the running level untouched.
        Common::BoolResultStr LoadSceneInternal( const std::string& path );

    private:
        std::string          m_ScenePathOverride;
        Core::PlayRequest    m_PlayRequest;
        Engine::Application* m_Application = nullptr;

        std::shared_ptr<Assets::AssetManager> m_AssetManager;
        // The boot's own record: one named, timed stage per boot function, plus the systems and the scene load.
        // A MEMBER AND NOT A LOCAL IN OnAttach, because the summary is logged after the scene has loaded
        // and a local would have gone out of scope with the stage list in it.
        Core::BootTimeline m_Boot{ "Runtime" };

        // The library the boot's "Indexing animation clips" stage fills (Assets::IndexAnimationClips).
        std::unique_ptr<Animation::AnimationLibrary> m_AnimationLibrary;
        // QualityBoot::Start's answer, taken in the constructor (before the renderer) and returned by OnAttach.
        Common::BoolResultStr                        m_QualityStart = Common::MakeSuccess( true );
        std::unique_ptr<Graphic::SceneRenderer>      m_SceneRenderer;
        std::shared_ptr<Core::Scene>                 m_Scene;
        // A partitioned world keeps only the camera's neighbourhood in the ECS (WorldStreamer.hpp); null for a
        // world without a WorldPartition block. Reset before the scene it streams is cleared.
        std::unique_ptr<Core::WorldStreamer> m_WorldStreamer;
        double                               m_WorldStreamClock = 0.0; // seconds of play, for retries

        // The present path: one graph node into the imported back buffer blits the scene's final image with
        // a fullscreen triangle, then draws the UI + splash with the engine's own Render2D batcher. Lazily
        // created on the first present (the pipelines are built against the swapchain's composite framebuffer).
        std::unique_ptr<Graphic::Render2D::Render2D> m_Render2D;

        // The offscreen worlds behind this game's render-texture UI elements (Ю16). HERE and not only in
        // the editor: this is the process that ships, and an element that works while authoring and draws
        // magenta in the pak is the worse of the two outcomes. unique_ptr and not by value for the same
        // reason m_Render2D is — the header forward-declares the Render2D namespace rather than pulling
        // the batcher in, and this type owns Scenes and SceneRenderers behind it.
        std::unique_ptr<Graphic::Render2D::UIRenderTextureCache> m_UIRenderTextures;
        std::shared_ptr<Graphic::GraphicsPipeline>   m_BlitPipeline;
        std::unique_ptr<Graphic::MaterialExecutor>   m_BlitExecutor;
        // The present node's blit block (RDG-FAULT1), keyed on m_BlitPipeline's shader.
        Graphic::ShaderBindingLayoutCache            m_BlitLayout;
        bool                                         m_PresentReady  = false;
        bool                                         m_PrevMouseDown = false; // for the click (down->up) edge
        float                                        m_ScrollAccum   = 0.0f;  // wheel delta since last present
        std::string                                  m_TypedText;             // chars typed since last present
        std::vector<UI::UIKeyEvent> m_UIKeys; // key presses since last present, in order (UIInput::Keys)
        entt::entity                                 m_FocusedUI     = entt::null; // the focused control (or null)

        // The player's one view: hover and tween clocks, the elected hot element, the drag, and one screen
        // stack per canvas the level holds. It rebinds itself when a "scene:" button loads another scene, so
        // entity ids from the old registry never answer for the new one, and a canvas destroyed mid-level
        // takes its cell with it.
        // The engine resources this view draws with; declared first, so it outlives the view built on it.
        UI::RegistryUICanvasResources m_UIResources;
        UI::UIViewContext             m_UIView{ m_UIResources };
        // This frame's step, recorded by OnUpdate for the UI walk in OnUIRender (which is handed no time):
        // UI::BeginUIFrame advances the view by exactly the step the host ticked, not by a clock of its own.
        float m_UIFrameDtSeconds = 0.0f;

        Common::BoolResultStr InitPresent( const std::shared_ptr<Graphic::Framebuffer>& swapFb );

        /// The render collectors and gameplay systems, in the order Play mode uses. Its own function so
        /// the boot stage that times it stays a one-line lambda — see the note on the definition.
        void BuildGameplaySystems();

        // Scene::Resize destroys GPU resources — deferred to the top of OnUpdate (same rule as the
        // editor's viewport panel).
        std::optional<std::pair<uint32_t, uint32_t>> m_PendingResize;

        // THE MOVIE RENDER (--render-movie, MovieRender.hpp). Set, the frame is composed into m_MovieTarget —
        // an offscreen framebuffer of the requested size — instead of the swapchain, and every frame drawn
        // after the content gate opened is read back and written as the next numbered PNG. Absent from a
        // Shipping build, with the flag.
#if DESERT_DEV_INSTRUMENTS
        std::optional<MovieRenderRequest>     m_Movie;
        std::shared_ptr<Graphic::Framebuffer> m_MovieTarget;
        uint32_t                              m_MovieFrame      = 0;     // index of the next PNG
        bool                                  m_MovieFrameDrawn = false; // this frame showed the world -> write it
        Common::BoolResultStr                 InitMovieTarget();
        void                                  CollectMovieFrame();
#endif
        uint32_t                                     m_LastWidth = 0, m_LastHeight = 0;

        // ===== THE STATE A SHIPPING GAME NEEDS AND DID NOT HAVE =====
        //
        // Demand-driven loading moved the read of the world's content out of the boot and into the first
        // frame that asks for it. The editor covers those frames with the loading overlay it already had;
        // this host had nothing, so the first frames of a packaged game could present a world whose sky
        // had not landed — and a volumetric cloud with no volume falls back to a PROCEDURAL sky, which
        // is a picture, not an error. Nothing would have been logged and nothing would have looked
        // broken. That is the worst shape a deficit can take, and it is the one a player gets.
        //
        // Constructed `Loading` on purpose: this process exists in order to read a world, and a gate
        // that defaulted to `Ready` would present exactly the frames it was added to cover. The rule
        // that closes it lives in one place for both hosts — Engine/Assets/ContentGate.hpp.
        Assets::ContentGate m_Content{ Assets::ContentState::Loading };

        /// How many frames the loading screen has been PRESENTED. Drives the activity strip (a still
        /// loading screen is indistinguishable from a hung game) and is what the settle log reports.
        uint32_t m_LoadingFramesPresented = 0;

        /// Drawn while `m_Content.Loading()` — an opaque cover, the scene's splash sprite if it names
        /// one, and a moving strip. Returns nothing: it cannot fail, and a world with no splash is a
        /// legitimate game, so an empty cover is the correct answer rather than a refusal.
        void DrawLoadingScreen( Graphic::Render2D::DrawList2D& dl, float w, float h );

        /// Everything that must happen exactly once, on the tick the gate opens: the game starts.
        void OnContentReady();

        // Counted in frames this host has PRESENTED, from 1. NOT part of the capture: the loading-state
        // log line reports it in every configuration, which is why it stays outside the guard below.
        uint32_t m_PresentedFrames = 0;

        // ===== Unattended capture of the PRESENTED frame (--shot / --shot-frames) =====
        // The copy is recorded into this frame's command buffer at the tail of OnUIRender and
        // collected in OnFramePresented. Absent from a Shipping build — see RuntimeShot.hpp.
#if DESERT_DEV_INSTRUMENTS
        bool m_ShotRecorded = false;
        /// Recorded into the command buffer if this frame is the one asked for. Nothing otherwise.
        void RecordShotIfDue();
#endif

        // Splash screen (SceneSettings.Splash*): a full-screen image shown when a scene loads, fading in/out.
        // Armed by TriggerSplash() on load; m_SplashTimer counts down each frame.
        Assets::AssetHandle m_SplashSprite;
        float               m_SplashTimer    = 0.0f;
        float               m_SplashDuration = 0.0f;
        float               m_SplashFade     = 0.4f;
        void                TriggerSplash();

        // ===== Startup movies (Config/Game.json StartupMovies; UE Project Settings ▸ Movies) =====
        // Full screen, one after another, from the first presented frame; the level is shown only once
        // they are over AND the world is complete. The sound is declared before the sequence because the
        // sequence's player holds a raw pointer to it; the texture outlives both until OnDetach, since the
        // frame that drew the last movie picture may still be sampling it when the sequence ends.
        std::unique_ptr<Media::MediaAudioOutput>   m_StartupSound;
        std::unique_ptr<Media::StartupMoviePlayer> m_StartupMovies;
        std::unique_ptr<Media::MediaTexture>       m_StartupPicture;
        bool                                       m_SkipStartupMovie = false; // a key / click since the last tick
        bool m_StartupMoviesStarted  = false; // on the tick after the first presented frame
        bool m_StartupPictureCurrent = false; // the movie texture holds the player's current frame this tick
        bool m_PrevAnyMouseDown      = false; // for the press edge that skips
        bool m_SplashAfterMovies     = false; // the world completed under a movie: its splash waits for the end
        void BeginStartupMovies();
        void TickStartupMovies( double deltaSeconds );
        [[nodiscard]] bool StartupMoviesPlaying() const;
        void               DrawStartupMovie( Graphic::Render2D::DrawList2D& dl, float w, float h );
        /// What a covered frame (loading screen, startup movie) does with input: drops it, so nothing
        /// pressed during the cover reaches the first frame the player can see.
        void DiscardHeldInput();
    };
} // namespace Desert::Player
