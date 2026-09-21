#pragma once

#include <Engine/Core/BootTimeline.hpp>
#include <Engine/Desert.hpp>
#include <Engine/UI/UICanvasContext.hpp>

#include <entt/entt.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Desert::Graphic
{
    class GraphicsPipeline;
    class MaterialExecutor;
} // namespace Desert::Graphic
namespace Desert::Graphic::Render2D
{
    class Render2D;
    class UIRenderTextureCache;
}

namespace Desert::Player
{
    // The PLAYER layer: loads the opened project's scene, flips it straight into Play (scripts, physics,
    // gameplay camera all live) and presents the rendered frame fullscreen through a chrome-less ImGui
    // window. No panels, no gizmos, no editing — the game, exactly as Play-in-editor runs it.
    // (namespace Player: Desert::Runtime already belongs to the engine's runtime services.)
    class RuntimeLayer : public Common::Layer
    {
    public:
        // scenePathOverride: from `--scene <path>`; empty -> the project's DefaultScene.
        // application: the owner, so a UI "quit" button can ask for an ordered close instead of calling
        // std::exit() from inside the frame (which destroys the job system's mutexes under its own live
        // worker threads — see the quit handler in OnUpdate).
        RuntimeLayer( std::string scenePathOverride, Engine::Application* application );
        ~RuntimeLayer();

        [[nodiscard]] Common::BoolResultStr OnAttach() override;
        [[nodiscard]] Common::BoolResultStr OnDetach() override;
        [[nodiscard]] Common::BoolResultStr OnUpdate( const Common::Timestep& ts ) override;
        [[nodiscard]] Common::BoolResultStr OnImGuiRender() override;
        void                                OnEvent( Common::Event& event ) override;

    private:
        // Tear down the current scene and deserialize `path` in its place (systems survive Clear()). Runs
        // between frames from OnUpdate — a UI button's "scene:<path>" click queues it into m_PendingSceneLoad.
        void LoadSceneInternal( const std::string& path );

    private:
        std::string          m_ScenePathOverride;
        Engine::Application* m_Application = nullptr;

        std::shared_ptr<Assets::AssetManager> m_AssetManager;
        // BEFORE the preloader, which holds a non-owning reference to it and must therefore not outlive
        // it: members are destroyed in reverse declaration order.
        // The boot's own record: one named, timed stage per preload, plus the systems and the scene load.
        // A MEMBER AND NOT A LOCAL IN OnAttach, because the summary is logged after the scene has loaded
        // and a local would have gone out of scope with the stage list in it.
        Core::BootTimeline m_Boot{ "Runtime" };

        std::unique_ptr<Animation::AnimationLibrary> m_AnimationLibrary;
        std::unique_ptr<Assets::AssetPreloader>      m_AssetPreloader;
        std::unique_ptr<Graphic::SceneRenderer>      m_SceneRenderer;
        std::shared_ptr<Core::Scene>                 m_Scene;

        // No-ImGui present: the runtime opens the swapchain pass itself, blits the scene's final image with a
        // fullscreen quad, then draws the UI + splash with the engine's own Render2D batcher. Lazily created on
        // the first present (the swapchain framebuffer only exists after the first BeginSwapChainRenderPass).
        std::unique_ptr<Graphic::Render2D::Render2D> m_Render2D;

        // The offscreen worlds behind this game's render-texture UI elements (Ю16). HERE and not only in
        // the editor: this is the process that ships, and an element that works while authoring and draws
        // magenta in the pak is the worse of the two outcomes. unique_ptr and not by value for the same
        // reason m_Render2D is — the header forward-declares the Render2D namespace rather than pulling
        // the batcher in, and this type owns Scenes and SceneRenderers behind it.
        std::unique_ptr<Graphic::Render2D::UIRenderTextureCache> m_UIRenderTextures;
        std::shared_ptr<Graphic::GraphicsPipeline>   m_BlitPipeline;
        std::unique_ptr<Graphic::MaterialExecutor>   m_BlitExecutor;
        bool                                         m_PresentReady  = false;
        bool                                         m_PrevMouseDown = false; // for the click (down->up) edge
        float                                        m_ScrollAccum   = 0.0f;  // wheel delta since last present
        std::string                                  m_TypedText;             // chars typed since last present
        bool                                         m_Backspace     = false; // backspace pressed since present
        bool                                         m_TabPressed    = false; // Tab pressed since present
        bool                                         m_SubmitPressed = false; // Enter pressed since present
        bool                                         m_EscapePressed = false; // Escape pressed since present
        entt::entity                                 m_FocusedUI     = entt::null; // the focused control (or null)

        // The player's one view: hover and tween clocks, the elected hot element, the drag, and one screen
        // stack per canvas the level holds. It rebinds itself when a "scene:" button loads another scene, so
        // entity ids from the old registry never answer for the new one, and a canvas destroyed mid-level
        // takes its cell with it.
        UI::UIViewContext m_UIView;

        Common::BoolResultStr InitPresent( const std::shared_ptr<Graphic::Framebuffer>& swapFb );

        /// The render collectors and gameplay systems, in the order Play mode uses. Its own function so
        /// the boot stage that times it stays a one-line lambda — see the note on the definition.
        void BuildGameplaySystems();

        // Scene::Resize destroys GPU resources — deferred to the top of OnUpdate (same rule as the
        // editor's viewport panel).
        std::optional<std::pair<uint32_t, uint32_t>> m_PendingResize;
        uint32_t                                     m_LastWidth = 0, m_LastHeight = 0;

        // A UI button clicked this frame with an "scene:<path>" OnClickMessage — applied next OnUpdate.
        std::optional<std::string> m_PendingSceneLoad;

        // One-shot "startup is over" log marker (see OnUpdate) — the boundary startup timings end at.
        bool m_LoggedFirstUpdate = false;

        // Splash screen (SceneSettings.Splash*): a full-screen image shown when a scene loads, fading in/out.
        // Armed by TriggerSplash() on load; m_SplashTimer counts down each frame.
        // ===== Demand-driven content settling (see OnUpdate) =====
        // `AsyncAssetLoader::StartedCount()` as it stood at the start of the frame just rendered, and
        // how many frames the wait has taken. Same two-condition rule as the editor's, and the same
        // reason: an empty queue in the middle of a chain is not a settled one.
        uint64_t m_ContentStartedAtFrameBegin = 0;
        uint32_t m_ContentSettleFrames        = 0;

        Assets::AssetHandle m_SplashSprite;
        float               m_SplashTimer    = 0.0f;
        float               m_SplashDuration = 0.0f;
        float               m_SplashFade     = 0.4f;
        void                TriggerSplash();
    };
} // namespace Desert::Player
