#pragma once

#include <Engine/Core/Application.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Core/Device.hpp>
#include <Engine/Core/FrameManager.hpp>

#include <Engine/Graphic/RendererContext.hpp>

namespace Desert
{
    /**
     * @brief The EngineContext acts as a central access point for main engine subsystems.
     *        It manages the lifecycle and provides access to Window, Device, and RendererContext.
     */
    class EngineContext final : public Common::Singleton<EngineContext>
    {
    public:
        void Initialize( const std::shared_ptr<Window>& window, 
                         const std::shared_ptr<Engine::Device>& device,
                         const std::shared_ptr<Graphic::RendererContext>& rendererContext )
        {
            m_Window          = window;
            m_Device          = device;
            m_RendererContext = rendererContext;

            // Initialize FrameManager with default value, can be overridden by renderer
            Engine::FrameManager::CreateInstance().Initialize( 2 );
        }

        void SetDevice( const std::shared_ptr<Engine::Device>& device )
        {
            m_Device = device;
        }

        /**
         * @brief Returns the current frame index from FrameManager.
         */
        [[nodiscard]] uint32_t GetCurrentFrameIndex() const
        {
            return Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        }

        [[nodiscard]] uint32_t GetMaxFramesInFlight() const
        {
            return Engine::FrameManager::GetInstance().GetMaxFramesInFlight();
        }

        [[nodiscard]] std::shared_ptr<Window> GetWindow() const
        {
            return m_Window.lock();
        }

        [[nodiscard]] std::shared_ptr<Engine::Device> GetDevice() const
        {
            return m_Device.lock();
        }

        [[nodiscard]] std::shared_ptr<Graphic::RendererContext> GetRendererContext() const
        {
            return m_RendererContext.lock();
        }

        /**
         * @brief Utility for accessing device capabilities directly.
         */
        [[nodiscard]] const Engine::DeviceCapabilities& GetCapabilities() const
        {
            auto device = GetDevice();
            DESERT_VERIFY( device != nullptr );
            return device->GetCapabilities();
        }

        /**
         * @brief Utility for GLFW window handle.
         */
        [[nodiscard]] void* GetNativeWindowHandle() const
        {
            if ( auto window = m_Window.lock() )
                return const_cast<void*>( window->GetNativeWindow() );
            return nullptr;
        }

    private:
        std::weak_ptr<Window>                   m_Window;
        std::weak_ptr<Engine::Device>           m_Device;
        std::weak_ptr<Graphic::RendererContext> m_RendererContext;

    private:
        friend class Desert::Engine::Application;
    };

} // namespace Desert
