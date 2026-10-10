#include <Engine/UI/Ecs/EcsUITree.hpp>
#include <Engine/UI/Ecs/UICanvasLayoutEcs.hpp>

#include <vector>

// The registry-signature layout queries every engine and editor caller already uses. Each one wraps the
// registry in an EcsUITree and asks the tree overload in UICanvasLayout.cpp — there is ONE walk, and this
// file only converts ids (bit for bit, see EcsUITree.hpp).
namespace Desert::UI
{
    entt::entity CanvasOf( entt::registry& reg, entt::entity e )
    {
        return ToEntity( CanvasOf( EcsUITree( reg ), ToNode( e ) ) );
    }

    std::size_t CanvasCount( entt::registry& reg )
    {
        return CanvasCount( EcsUITree( reg ) );
    }

    Common::ResultStr<entt::entity> SoleCanvas( entt::registry& reg )
    {
        const auto sole = SoleCanvas( EcsUITree( reg ) );
        if ( !sole )
            return Common::MakeError<entt::entity>( sole.GetError() );
        return Common::MakeSuccess( ToEntity( sole.GetValue() ) );
    }

    std::vector<entt::entity> CanvasesInDrawOrder( entt::registry& reg )
    {
        const std::vector<NodeId> nodes = CanvasesInDrawOrder( EcsUITree( reg ) );
        std::vector<entt::entity> out;
        out.reserve( nodes.size() );
        for ( const NodeId n : nodes )
            out.push_back( ToEntity( n ) );
        return out;
    }

    bool TakesLayoutSpace( entt::registry& reg, entt::entity e )
    {
        return TakesLayoutSpace( EcsUITree( reg ), ToNode( e ) );
    }

    bool IsElementVisible( entt::registry& reg, entt::entity e )
    {
        return IsElementVisible( EcsUITree( reg ), ToNode( e ) );
    }

    Common::BoolResultStr EnumerateCanvas( entt::registry& reg, entt::entity canvas, const Rect& viewportPx,
                                           std::vector<UIElementNode>& out, const UICanvasContext* ctx )
    {
        return EnumerateCanvas( EcsUITree( reg ), ToNode( canvas ), viewportPx, out, ctx );
    }

    bool BindingHidesElement( entt::registry& reg, entt::entity e, const UICanvasContext* ctx,
                              const UIDataStore* row )
    {
        return BindingHidesElement( EcsUITree( reg ), ToNode( e ), ctx, row );
    }

    entt::entity PickElement( entt::registry& reg, entt::entity canvas, const glm::vec2& pointPx,
                              const Rect& viewportPx )
    {
        return ToEntity( PickElement( EcsUITree( reg ), ToNode( canvas ), pointPx, viewportPx ) );
    }

    bool GetElementRect( entt::registry& reg, entt::entity canvas, entt::entity target, const Rect& viewportPx,
                         Rect& out, glm::mat3* outXform )
    {
        return GetElementRect( EcsUITree( reg ), ToNode( canvas ), ToNode( target ), viewportPx, out, outXform );
    }

    Common::ResultStr<float> CanvasScale( entt::registry& reg, entt::entity canvas, const Rect& viewportPx )
    {
        return CanvasScale( EcsUITree( reg ), ToNode( canvas ), viewportPx );
    }
} // namespace Desert::UI
