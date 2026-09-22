#pragma once

#include <Common/Core/Layer.hpp>

#include <memory>

// THE IMGUI INTEGRATION LIVES IN THE EDITOR, NOT IN THE ENGINE.
//
// Dear ImGui is the EDITOR's interface toolkit and nothing else's: the packaged Runtime draws its UI with
// the engine's own Render2D batcher (RuntimeLayer::OnUIRender), and no engine translation unit names a
// single ImGui symbol. These files used to sit in Engine/imgui/ and Engine/Graphic/API/Vulkan/imgui/,
// which made `libDesert.a` — and therefore the shipped player — carry an interface toolkit it never
// draws with. `Desert/Tests/Engine/ImGuiBoundary` is the census that keeps them out.
//
// The namespace is still `Desert::ImGui` rather than `Desert::Editor::ImGui` ON PURPOSE. Editor code
// spells the library's own namespace `::ImGui` and aliases it locally (`namespace ImGui = ::ImGui;`)
// precisely because `Desert::ImGui` already shadows it; moving this type INTO `Desert::Editor` would make
// every unqualified `ImGui::Button` inside `Desert::Editor` resolve here instead, which is a compile
// error at best and the wrong overload at worst. The move is about the build graph, not about spelling.
namespace Desert::ImGui
{
    class ImGuiLayer : public Common::Layer
    {
    public:
        virtual void Begin() = 0;
        virtual void End()   = 0;

        static std::shared_ptr<ImGuiLayer> Create();
    };

} // namespace Desert::ImGui
