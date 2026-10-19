#pragma once

#include <Engine/Graphic/Environment/OwnedEnvironment.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/SkyRules.hpp>

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <memory>

namespace Desert::Graphic
{
    /// What a procedural environment was baked from: the sun, and the two fingerprints the sun is not
    /// (Graphic::CloudEnvironmentFingerprint, Graphic::SkyBakeFingerprint). Stamped only by a bake that LANDED —
    /// a failed bake that stamped its inputs would look up to date forever.
    struct SkyBakeInputs
    {
        glm::vec3 SunDir           = glm::vec3( 0.0f, 1.0f, 0.0f ); // TOWARD the sun, normalized
        uint64_t  CloudFingerprint = 0; // 0 = "no clouds", also the value of a scene that never had any
        uint64_t  SkyFingerprint   = 0;
    };

    /// THE SCENE'S PROCEDURAL SKY ENVIRONMENT (UE: FScene's sky light capture — one real-time capture per scene,
    /// read by every view of it). One per Core::Scene (Scene::GetSkyEnvironment), never per view: two views of
    /// one scene neither bake nor hold two copies of the same sky.
    ///
    /// Who calls it: SkyboxRenderer::EnsureProceduralEnvironment, from SceneRenderer::OnUpdate, before the
    /// view's graph records. The FIRST view of a frame (BeginViewFrame) drives the trigger and the debounce and,
    /// when a rebake is due, issues it with its own LUTs and parameter buffers; the views after it read the
    /// environment that view left. SkyboxRenderer::GetEnvironment reads it every frame through a weak_ptr — a view
    /// never keeps a scene alive, and a view bound to another scene reads that scene's.
    ///
    /// Ledger: every row the bake opens is the scene's (ResourceOwner::Environment, GetLedgerInstance()), so the
    /// environment is in the scene's budget entry once, whatever number of views draws it.
    ///
    /// Never: hold a view's object; land a bake before its batch is complete. The issuing view settles a bake it
    /// left on the GPU before its buffers die (SkyboxRenderer's destructor, SettlePendingBake): the batch reads
    /// them.
    class SceneSkyEnvironment
    {
    public:
        explicit SceneSkyEnvironment( ResourceOwnerInstance scene ) : m_Scene( scene )
        {
        }

        SceneSkyEnvironment( const SceneSkyEnvironment& )            = delete;
        SceneSkyEnvironment& operator=( const SceneSkyEnvironment& ) = delete;
        SceneSkyEnvironment( SceneSkyEnvironment&& )                 = delete;
        SceneSkyEnvironment& operator=( SceneSkyEnvironment&& )      = delete;

        /// The scene the environment's ledger rows belong to.
        [[nodiscard]] ResourceOwnerInstance GetLedgerInstance() const
        {
            return m_Scene;
        }

        /// Every view calls this with the engine's absolute frame number before it touches the bake. True for the
        /// first view of @p frame (it drives the trigger, the debounce and the bake), false for the views after
        /// it.
        [[nodiscard]] bool BeginViewFrame( uint64_t frame );

        /// The editor's one-shot "Bake" request, from whichever view's settings path saw it.
        void RequestBake()
        {
            m_BakeRequested = true;
        }
        /// Reads and clears the request.
        [[nodiscard]] bool TakeBakeRequest();

        /// Debounce: how long the sun has held still (see SkyEnvironmentRebakeMayRun). Compared by direction
        /// rather than by "did anything write it", because the time-of-day driver rewrites the same value every
        /// frame when it is paused.
        void                ObserveSun( const glm::vec3& sunDir, float deltaSeconds );
        [[nodiscard]] float SecondsSinceSunMoved() const
        {
            return m_SecondsSinceSunMoved;
        }
        /// How long a rebake has been wanted; stops a sun that never stops from deferring it forever.
        [[nodiscard]] float SecondsSinceStale() const
        {
            return m_SecondsSinceStale;
        }
        void AccumulateStale( float deltaSeconds )
        {
            m_SecondsSinceStale += deltaSeconds;
        }
        void ResetStale()
        {
            m_SecondsSinceStale = 0.0f;
        }

        [[nodiscard]] bool HasPendingBake() const
        {
            return m_PendingBake != nullptr;
        }
        /// Lands the bake on the GPU if its batch is complete. True when nothing is pending afterwards.
        bool LandPendingBake();
        /// Blocks until the pending bake (if any) is complete, then lands it.
        void SettlePendingBake();
        /// The bake a view just issued; landed in the same call when the GPU is already done.
        void Submit( std::unique_ptr<ProceduralEnvironmentBake> bake, const SkyBakeInputs& inputs,
                     SkyEnvironmentSize size, const char* what );

        /// What the environment on the device was baked from (see SkyBakeInputs).
        [[nodiscard]] const SkyBakeInputs& GetBaked() const
        {
            return m_Baked;
        }
        [[nodiscard]] bool HasEnvironment() const
        {
            return static_cast<bool>( m_ProceduralEnv );
        }
        [[nodiscard]] const Environment& GetEnvironment() const
        {
            return m_ProceduralEnv.Get();
        }
        /// "The light this scene's sky gives is not the one it will give a frame from now": a bake still on the
        /// GPU, or none yet.
        [[nodiscard]] bool IsSettling() const
        {
            return m_PendingBake != nullptr || !m_ProceduralEnv;
        }

        /// True exactly once per scene: the High-resolution memory report is announced once, not per bake.
        [[nodiscard]] bool TakeFirstHighResReport();

    private:
        ResourceOwnerInstance m_Scene;

        uint64_t m_Frame    = 0;
        bool     m_HasFrame = false;

        bool m_BakeRequested     = false;
        bool m_HighResCostLogged = false;

        glm::vec3 m_LastSeenSunDir       = glm::vec3( 0.0f, 1.0f, 0.0f );
        float     m_SecondsSinceSunMoved = 0.0f;
        float     m_SecondsSinceStale    = 0.0f;

        // The baked sky IBL (radiance/irradiance/prefiltered cubes). OWNED, not a copy of handles: its cubes live
        // in the process-wide ImageService, and the scene is the only one that can release them — when it
        // rebakes, and when it dies (see OwnedEnvironment).
        OwnedEnvironment m_ProceduralEnv;
        SkyBakeInputs    m_Baked;

        // THE BAKE ON THE GPU, and what it is baked from — stamped into m_Baked only when it lands. Declared LAST
        // so it is destroyed FIRST: its destructor waits for the batch.
        std::unique_ptr<ProceduralEnvironmentBake> m_PendingBake;
        SkyBakeInputs                              m_Pending;
        SkyEnvironmentSize                         m_PendingSize{};
        const char*                                m_PendingWhat = "";
        std::chrono::steady_clock::time_point      m_PendingStarted{};
    };
} // namespace Desert::Graphic
