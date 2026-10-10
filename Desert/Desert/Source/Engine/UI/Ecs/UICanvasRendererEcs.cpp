#include <Engine/UI/Ecs/EcsUITree.hpp>
#include <Engine/UI/Ecs/UICanvasRendererEcs.hpp>

// The registry-signature frame entry points every engine and editor caller already uses. Each one wraps the
// registry in an EcsUITree and calls the tree overload — there is ONE walk, and this file only converts ids
// (bit for bit, see EcsUITree.hpp). The focused element travels both ways, so it is converted in and out.
namespace Desert::UI
{
    void BeginUIFrame( UIViewContext& view, entt::registry& reg, const Rect& viewportPx, float frameDtSeconds )
    {
        EcsUITree tree( reg );
        BeginUIFrame( view, tree, viewportPx, frameDtSeconds );
    }

    void EndUIFrame( UIViewContext& view, entt::registry& reg, Graphic::Render2D::DrawList2D& dl,
                     const UIInput* input, entt::entity* focused, std::string* outClicked,
                     std::vector<std::string>* outMessages )
    {
        EcsUITree tree( reg );
        NodeId    node = focused != nullptr ? ToNode( *focused ) : NodeId::Null;
        EndUIFrame( view, tree, dl, input, focused != nullptr ? &node : nullptr, outClicked, outMessages );
        if ( focused != nullptr )
            *focused = ToEntity( node );
    }

    Common::BoolResultStr RenderCanvas2D( UIViewContext& view, entt::registry& reg, entt::entity canvas,
                                          Graphic::Render2D::DrawList2D& dl, const glm::mat4* worldViewProj,
                                          const UIInput* input, std::string* outClicked, entt::entity* focused )
    {
        EcsUITree tree( reg );
        NodeId    node   = focused != nullptr ? ToNode( *focused ) : NodeId::Null;
        auto      result = RenderCanvas2D( view, tree, ToNode( canvas ), dl, worldViewProj, input, outClicked,
                                      focused != nullptr ? &node : nullptr );
        if ( focused != nullptr )
            *focused = ToEntity( node );
        return result;
    }

    Common::ResultStr<entt::entity> OverlayByName( entt::registry& reg, const std::string& name )
    {
        EcsUITree  tree( reg );
        const auto found = OverlayByName( tree, name );
        if ( !found )
            return Common::MakeError<entt::entity>( found.GetError() );
        return Common::MakeSuccess( ToEntity( found.GetValue() ) );
    }
} // namespace Desert::UI
