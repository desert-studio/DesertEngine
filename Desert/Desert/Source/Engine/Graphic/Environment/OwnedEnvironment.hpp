#pragma once

#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Runtime/Services/Image/ImageService.hpp>

#include <utility>

namespace Desert::Graphic
{
    /**
     * @brief A baked Environment whose cubes belong to the view that baked it, and die with it.
     *
     * WHY: EnvironmentManager::CreateProcedural registers the irradiance and prefiltered cubes in the
     * process-wide ImageService, which holds them until someone unregisters. SkyboxRenderer released the
     * PREVIOUS environment on a rebake and nothing released the LAST one, so every closed view with a sky
     * left 8.16 MiB (EnvPrefiltered 8.06 + EnvDiffuseIrradiance 0.09) resident for the rest of the session —
     * RT2k measured it as +8 of each per eight viewport open-close cycles in the allocator's ledger, while
     * every view's own ledger stayed flat.
     *
     * The rule is the view-resources one: what a view creates, the view releases. Unregistering drops the
     * service's reference; the image's destructor hands the VkImage to the allocator's deferred deletion
     * queue, so this is safe at any point of a frame.
     */
    class OwnedEnvironment
    {
    public:
        OwnedEnvironment() = default;
        ~OwnedEnvironment()
        {
            Release();
        }

        OwnedEnvironment( const OwnedEnvironment& )            = delete;
        OwnedEnvironment& operator=( const OwnedEnvironment& ) = delete;
        OwnedEnvironment( OwnedEnvironment&& )                 = delete;
        OwnedEnvironment& operator=( OwnedEnvironment&& )      = delete;

        /// Takes ownership of @p baked (registered in @p service) and releases the environment held before.
        void Replace( Runtime::ImageService& service, Environment baked )
        {
            Release();
            m_Service     = &service;
            m_Environment = std::move( baked );
        }

        /// Unregisters every cube this object owns and leaves it empty. Idempotent.
        void Release()
        {
            if ( m_Service != nullptr )
            {
                m_Service->Unregister( m_Environment.RadianceMap );
                m_Service->Unregister( m_Environment.IrradianceMap );
                m_Service->Unregister( m_Environment.PreFilteredMap );
            }
            m_Service     = nullptr;
            m_Environment = {};
        }

        [[nodiscard]] const Environment& Get() const noexcept
        {
            return m_Environment;
        }

        explicit operator bool() const
        {
            return static_cast<bool>( m_Environment );
        }

    private:
        Runtime::ImageService* m_Service = nullptr;
        Environment            m_Environment;
    };
} // namespace Desert::Graphic
