#pragma once

#include <Common/Core/Events/Event.hpp>
#include <Common/Core/Timestep.hpp>
#include <Common/Core/ResultStr.hpp>

#include <string>

namespace Common
{
    class Layer
    {
    public:
        Layer( const std::string& name = "DebugName" );
        virtual ~Layer()
        {
        }

        [[nodiscard]] virtual Common::BoolResultStr OnAttach()                             = 0;
        [[nodiscard]] virtual Common::BoolResultStr OnDetach()                             = 0;
        [[nodiscard]] virtual Common::BoolResultStr OnUpdate( const Common::Timestep& ts ) = 0;

        /**
         * @brief THE per-frame interface hook, run after every layer's OnUpdate and before the frame is
         *        submitted. What a layer draws in it is the layer's own business.
         *
         * IT WAS CALLED `OnImGuiRender`, AND THE NAME WAS A LIE IN THE SHIPPED PRODUCT. There are two
         * implementations. `EditorLayer` records Dear ImGui into the swapchain pass; `RuntimeLayer` opens
         * that pass itself and draws the game's UI canvas and splash with the engine's own Render2D
         * batcher — no ImGui anywhere in the process. The runtime's override used to carry a comment
         * apologising for the name; the name is fixed instead, so there is nothing left to apologise for.
         *
         * ONE HOOK, NOT TWO. A second per-frame UI callback ("OnEditorUI" beside "OnGameUI") would be a
         * second source of truth for "when in the frame the interface is recorded", and the ordering
         * between them would be a property of Application::Run that nothing states.
         */
        [[nodiscard]] virtual Common::BoolResultStr OnUIRender() = 0;

        virtual void OnEvent( Common::Event& event ) = 0;

        /**
         * @brief The frame is SUBMITTED AND PRESENTED. Called once per iteration, after the swapchain
         *        image has gone out; a no-op unless a layer needs that exact instant.
         *
         * WHY THE INSTANT IS WORTH A CALLBACK. Everything else in the loop happens while the frame is
         * still being built: OnUpdate renders the scene into an offscreen image, OnUIRender records the
         * interface into the swapchain render pass, and neither has been submitted yet. There is exactly
         * one point at which "the picture the user is looking at" exists as bytes on the device, and it is
         * after PresentFinalImage — which is where this runs.
         *
         * That is what the editor's control channel reads back for a full-window capture (the scene's own
         * final image holds no interface at all: ImGui draws into the swapchain, VulkanImGuiLayer::End), and
         * it is where the channel releases a reply, so that a reply provably follows a frame that already
         * reflects the command it answers.
         *
         * The alternative was to remember the swapchain buffer index at the end of OnUIRender and read
         * it back at the top of the next OnUpdate — which works only as long as the acquire/present index
         * bookkeeping keeps behaving as it does today, with three frames in flight. A callback at the point
         * itself states the requirement instead of depending on it.
         */
        virtual void OnFramePresented()
        {
        }

        inline const std::string& GetName()
        {
            return m_Name;
        }

    private:
        std::string m_Name;
    };
} // namespace Common