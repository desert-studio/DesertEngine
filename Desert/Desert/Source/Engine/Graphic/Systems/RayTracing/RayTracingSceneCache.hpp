#pragma once

#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometryVariants.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingProceduralGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSkinCache.hpp>

#include <cstdint>

// THE SCENE'S RAY-TRACING GEOMETRY (UE FScene's FGPUSkinCache and its cached ray-tracing geometry): what every
// view of one Core::Scene traces identically, made ONCE per frame and shared. Each view keeps its own TLAS (its
// instances are its own GPU-scene rows and its ISM distance cull is its own), but
//   * a skinned primitive is skinned and its BLAS built / refit once per frame, by the first view that asks; the
//     next views of the frame instance that same BLAS (RayTracingSkinCache);
//   * a (mesh, hidden mask) BLAS variant is built once for every view (RayTracingGeometryVariants).
// Main thread. Core::Scene owns one (GetRayTracingCache); a SceneRenderer reaches it between BeginScene and
// EndScene, the way it reaches the scene's reflection captures.
namespace Desert::Graphic::RayTracing
{
    class RayTracingSceneCache
    {
    public:
        // @p scene is the owning Core::Scene's ledger instance (Scene::GetLedgerInstance).
        explicit RayTracingSceneCache( const ResourceOwnerInstance scene ) : m_LedgerInstance( scene ) {}

        // Every view calls this before it gathers, with the engine's absolute frame number. A new number ends the
        // previous frame: the variants and skin-cache entries no view asked for during it are released (their
        // API objects retire on the frame ring, so a frame in flight still traces them safely).
        void BeginViewFrame( const uint64_t frame )
        {
            if ( m_HasFrame && frame == m_Frame )
                return;
            if ( m_HasFrame )
            {
                m_Variants.EndFrame();
                m_SkinCache.EndFrame( m_Frame );
                m_Procedural.EndFrame( m_Frame );
            }
            m_Frame    = frame;
            m_HasFrame = true;
        }

        [[nodiscard]] uint64_t GetFrame() const
        {
            return m_Frame;
        }
        [[nodiscard]] RayTracingGeometryVariants& GetVariants()
        {
            return m_Variants;
        }
        [[nodiscard]] const RayTracingGeometryVariants& GetVariants() const
        {
            return m_Variants;
        }
        [[nodiscard]] RayTracingSkinCache& GetSkinCache()
        {
            return m_SkinCache;
        }
        [[nodiscard]] const RayTracingSkinCache& GetSkinCache() const
        {
            return m_SkinCache;
        }
        [[nodiscard]] RayTracingProceduralCache& GetProcedural()
        {
            return m_Procedural;
        }
        [[nodiscard]] const RayTracingProceduralCache& GetProcedural() const
        {
            return m_Procedural;
        }

        // THE SCENE in the resource ledger (RT-LEDGER-2): every device object this cache's members create — BLAS
        // variants, skin-cache BLASes and positions, procedural BLASes — is a row of ResourceOwner::SceneRenderer
        // AND of this instance, opened under OpenLedgerScope(). A view's own RT rows carry the view's instance
        // instead, so the two are told apart: the scene's rows are its own budget entry (SceneRenderer::SceneHoldings),
        // never counted in any one view's.
        [[nodiscard]] ResourceOwnerInstance GetLedgerInstance() const
        {
            return m_LedgerInstance;
        }
        // The scope a view opens around every call into this cache that may create a device object. Nested
        // inside the view's own scope (SceneRenderer's RT block); the view's scope is restored when it closes.
        [[nodiscard]] ResourceAttributionScope OpenLedgerScope() const
        {
            return ResourceAttributionScope( ResourceOwner::SceneRenderer, m_LedgerInstance );
        }

    private:
        ResourceOwnerInstance      m_LedgerInstance; // the owning Core::Scene's (Scene::GetLedgerInstance)
        RayTracingGeometryVariants m_Variants{ MakeVariantBuild() };
        RayTracingSkinCache        m_SkinCache;
        RayTracingProceduralCache  m_Procedural; // BLASes of meshes the uploader built none for
        uint64_t                   m_Frame    = 0;
        bool                       m_HasFrame = false;
    };
} // namespace Desert::Graphic::RayTracing
