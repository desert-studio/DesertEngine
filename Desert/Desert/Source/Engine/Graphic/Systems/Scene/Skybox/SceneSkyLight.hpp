#pragma once

#include <Engine/Graphic/Environment/OwnedEnvironment.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/SkyRules.hpp>
#include <Engine/Graphic/Systems/Scene/Skybox/SkyCapture.hpp>

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>

namespace Desert::Graphic
{
    class FrameTextures;
    namespace RDG
    {
        class Builder;
    }

    /// What a sky light was captured from (Graphic::ShouldRebakeSkyEnvironment's three keys): the sun by angle,
    /// the clouds and the sky's own block by fingerprint. Stamped only by a capture that completed -- a failed one
    /// that stamped its inputs would look up to date forever.
    struct SkyLightStamps
    {
        glm::vec3 SunDir           = glm::vec3( 0.0f, 1.0f, 0.0f );
        uint64_t  CloudFingerprint = 0;
        uint64_t  SkyFingerprint   = 0;
    };

    /**
     * @brief THE SKY LIGHT OF ONE SCENE (UE: FScene's SkyLight with its real-time capture,
     * FScene::AllocateAndCaptureFrameSkyEnvMap and ConvolvedSkyRenderTarget[2]).
     *
     * Owned by the scene (Core::Scene), as UE's is by FScene: ONE capture of the procedural sky's light, shared by
     * every view of the scene -- each viewport, and the six face views of the reflection capture being rendered.
     * Before this, every SceneRenderer captured its own, so the first frame of a scene with reflection captures
     * paid seven captures of one sky (one per face view and the viewport's), and a second viewport a second
     * capture of the same light.
     *
     * WHICH VIEW RECORDS. The capture's passes go into the graph of the FIRST view of the frame that asks
     * (ClaimFrame): that view decides whether a capture begins, and records this frame's slice of it in its own
     * graph, at the head (SkyboxRenderer::AddSkyCapturePasses). The views after it in the frame read the
     * environment as that graph left it: their graphs execute after it on the same queue, and the images carry
     * their layouts across graphs, as the reflection captures' shared array does. A view that asks later in the
     * same frame neither decides nor records.
     *
     * The panorama step reads the claiming view's own per-view inputs -- its sky block, cloud blocks and
     * atmosphere LUTs (all on that renderer's slot, written in that frame) -- which are the same sky for every
     * view of one scene: the fingerprints that decide a recapture leave out the view's exposure
     * (Graphic::SkyBakeFingerprint), so two views never disagree about whether the sky changed.
     */
    class SceneSkyLight
    {
    public:
        SceneSkyLight()                                  = default;
        SceneSkyLight( const SceneSkyLight& )            = delete;
        SceneSkyLight& operator=( const SceneSkyLight& ) = delete;

        /// Once per frame, by every view whose sky is procedural, before its graph is built: true for the first
        /// one of this frame -- the view that decides and records the capture this frame -- and false for the
        /// others. The first call of a frame also advances the sun's hold-still clock by @p deltaSeconds.
        [[nodiscard]] bool ClaimFrame( const void* view, const glm::vec3& sunDir, float deltaSeconds );

        /// The Bake button, from any view: read by the next frame's claiming view.
        void RequestCapture()
        {
            m_CaptureRequested = true;
        }
        [[nodiscard]] bool TakeCaptureRequest()
        {
            const bool requested = m_CaptureRequested;
            m_CaptureRequested   = false;
            return requested;
        }

        /// The capture's compute programs, made on first use (a scene that never shows a procedural sky never
        /// makes them). False, said once, when one is missing.
        [[nodiscard]] bool EnsurePrograms();

        [[nodiscard]] bool IsCapturing() const
        {
            return m_Capture.IsCapturing();
        }
        [[nodiscard]] bool HasEnvironment() const
        {
            return static_cast<bool>( m_Environment );
        }
        /// The environment every view of the scene lights with: the last capture that completed.
        [[nodiscard]] const Environment& GetEnvironment() const
        {
            return m_Environment.Get();
        }
        [[nodiscard]] const SkyLightStamps& GetCaptured() const
        {
            return m_Captured;
        }

        /// The debounce (Graphic::SkyEnvironmentRebakeMayRun): how long the sun has held still, and how long a
        /// recapture has been wanted. Read and reset by the claiming view only.
        [[nodiscard]] float GetSecondsSinceSunMoved() const
        {
            return m_SecondsSinceSunMoved;
        }
        [[nodiscard]] float AccumulateStaleSeconds( float deltaSeconds )
        {
            m_SecondsSinceStale += deltaSeconds;
            return m_SecondsSinceStale;
        }
        void ClearStale()
        {
            m_SecondsSinceStale = 0.0f;
        }

        /// Begins a capture from the claiming view's @p source. The scene's first capture is whole -- recorded
        /// complete in this frame's graph, so the frame is lit by its sky -- and every later one is time-sliced
        /// while the previous environment lights the scene. @p what is the landing line's description.
        [[nodiscard]] bool Begin( const SkyEnvironmentSize& size, const System::SkyCaptureSource& source,
                                  const SkyLightStamps& from, const char* what );

        /// GRAPH BUILD, by @p view's graph: this frame's slice of the running capture, only when @p view claimed
        /// the frame. The frame its last step is recorded, the environment is replaced -- before this view
        /// imports it, and for every view after it.
        void AddPasses( const void* view, RDG::Builder& graph, FrameTextures& textures,
                        const System::SkyCaptureFrameReads& reads );

    private:
        [[nodiscard]] bool Claims( const void* view ) const;

        System::SkyCapture m_Capture;
        bool               m_ProgramsMade   = false;
        bool               m_ProgramsFailed = false;

        // The environment every view reads. OWNED: its cubes live in the process-wide ImageService, released when
        // a capture replaces them and when the scene dies (OwnedEnvironment).
        OwnedEnvironment m_Environment;
        SkyLightStamps   m_Captured;
        bool             m_CaptureRequested = false;

        // The frame's claim: the view that decides and records, and the frame it claimed.
        const void* m_Claimant   = nullptr;
        uint64_t    m_ClaimFrame = ~uint64_t{ 0 };

        // Debounce state (see SkyEnvironmentRebakeMayRun), advanced once per frame by the claim.
        glm::vec3 m_LastSeenSunDir       = glm::vec3( 0.0f, 1.0f, 0.0f );
        float     m_SecondsSinceSunMoved = 0.0f;
        float     m_SecondsSinceStale    = 0.0f;

        // The running capture: what it was begun from, and for the landing line its size, description, start
        // and the frames it has spanned.
        SkyLightStamps                        m_Pending;
        SkyEnvironmentSize                    m_PendingSize{};
        const char*                           m_PendingWhat = "";
        std::chrono::steady_clock::time_point m_PendingStarted{};
        uint32_t                              m_PendingFrames = 0;
    };
} // namespace Desert::Graphic
