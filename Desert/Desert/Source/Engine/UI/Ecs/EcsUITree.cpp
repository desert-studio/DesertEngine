#include "EcsUITree.hpp"

#include <Engine/ECS/Components.hpp>

#include <algorithm>

namespace Desert::UI
{
    namespace
    {
        // The one table from an argument kind to the ECS component that stores it. Each row is checked at
        // compile time against the Data's own `Arg`, so a row that names the wrong component cannot build.
        template <class C, ArgKind K>
        struct Row
        {
            using Component = C;
            static_assert( decltype( C::Data )::Arg == K, "the component's Data names a different ArgKind" );
        };

        template <class F>
        decltype( auto ) Dispatch( ArgKind kind, F&& f )
        {
            using enum ArgKind;
            switch ( kind )
            {
                case Layout:
                    return f( Row<ECS::UILayoutComponent, Layout>{} );
                case LayoutGroup:
                    return f( Row<ECS::UILayoutGroupComponent, LayoutGroup>{} );
                case Canvas:
                    return f( Row<ECS::UICanvasComponent, Canvas>{} );
                case Panel:
                    return f( Row<ECS::UIPanelComponent, Panel>{} );
                case Text:
                    return f( Row<ECS::UITextComponent2D, Text>{} );
                case Icon:
                    return f( Row<ECS::UIIconComponent, Icon>{} );
                case Image:
                    return f( Row<ECS::UIImageComponent, Image>{} );
                case RenderTexture:
                    return f( Row<ECS::UIRenderTextureComponent, RenderTexture>{} );
                case Button:
                    return f( Row<ECS::UIButtonComponent, Button>{} );
                case Toggle:
                    return f( Row<ECS::UIToggleComponent, Toggle>{} );
                case Slider:
                    return f( Row<ECS::UISliderComponent, Slider>{} );
                case InputField:
                    return f( Row<ECS::UIInputFieldComponent, InputField>{} );
                case Dropdown:
                    return f( Row<ECS::UIDropdownComponent, Dropdown>{} );
                case ScrollView:
                    return f( Row<ECS::UIScrollViewComponent, ScrollView>{} );
                case ListView:
                    return f( Row<ECS::UIListViewComponent, ListView>{} );
                case ProgressBar:
                    return f( Row<ECS::UIProgressBarComponent, ProgressBar>{} );
                case Path:
                    return f( Row<ECS::UIPathComponent, Path>{} );
                case Retainer:
                    return f( Row<ECS::UIRetainerComponent, Retainer>{} );
                case Style:
                    return f( Row<ECS::UIStyleComponent, Style>{} );
                case Tween:
                    return f( Row<ECS::UITweenComponent, Tween>{} );
                case Binding:
                    return f( Row<ECS::UIBindingComponent, Binding>{} );
                case Screen:
                    return f( Row<ECS::UIScreenComponent, Screen>{} );
                case ScreenStack:
                    return f( Row<ECS::UIScreenStackComponent, ScreenStack>{} );
                case PointerEvents:
                    return f( Row<ECS::UIPointerEventsComponent, PointerEvents>{} );
                case Draggable:
                    return f( Row<ECS::UIDraggableComponent, Draggable>{} );
                case DropTarget:
                    return f( Row<ECS::UIDropTargetComponent, DropTarget>{} );
                case Overlay:
                    return f( Row<ECS::UIOverlayComponent, Overlay>{} );
                case OverlayTrigger:
                    return f( Row<ECS::UIOverlayTriggerComponent, OverlayTrigger>{} );
                case Navigation:
                    return f( Row<ECS::UINavigationComponent, Navigation>{} );
                case Count:
                    break;
            }
            return f( nullptr );
        }

        // The kinds a control writes its value back into — and no others (see IUITree::FindState).
        constexpr bool IsStateKind( ArgKind kind )
        {
            switch ( kind )
            {
                case ArgKind::Toggle:
                case ArgKind::Slider:
                case ArgKind::InputField:
                case ArgKind::Dropdown:
                case ArgKind::ScrollView:
                case ArgKind::ListView:
                    return true;
                default:
                    return false;
            }
        }

        // The entity's INDEX, with entt's version bits stripped off — the order the registry handed the
        // ids out, which for a level being loaded is the order the file lists them. The version bits have
        // to come off: a recycled id carries a bumped version in the high bits, so a raw comparison would
        // sort every node that ever reused an id after every node that did not, whenever they were made.
        std::uint32_t CreationIndex( entt::entity e )
        {
            using Traits = entt::entt_traits<std::underlying_type_t<entt::entity>>;
            return static_cast<std::uint32_t>( entt::to_integral( e ) & Traits::entity_mask );
        }
    } // namespace

    bool EcsUITree::Valid( NodeId n ) const
    {
        const entt::entity e = ToEntity( n );
        return e != entt::null && m_Reg->valid( e );
    }

    std::size_t EcsUITree::NodeBound() const
    {
        return m_Reg->size();
    }

    NodeId EcsUITree::Parent( NodeId n ) const
    {
        if ( !Valid( n ) )
            return NodeId::Null;
        const auto* rel = m_Reg->try_get<ECS::RelationshipComponent>( ToEntity( n ) );
        return rel != nullptr ? ToNode( rel->Parent ) : NodeId::Null;
    }

    std::size_t EcsUITree::ChildCount( NodeId n ) const
    {
        if ( !Valid( n ) )
            return 0;
        const auto* rel = m_Reg->try_get<ECS::RelationshipComponent>( ToEntity( n ) );
        return rel != nullptr ? rel->Children.size() : 0;
    }

    NodeId EcsUITree::ChildAt( NodeId n, std::size_t i ) const
    {
        const auto* rel = Valid( n ) ? m_Reg->try_get<ECS::RelationshipComponent>( ToEntity( n ) ) : nullptr;
        return rel != nullptr && i < rel->Children.size() ? ToNode( rel->Children[i] ) : NodeId::Null;
    }

    std::string_view EcsUITree::Name( NodeId n ) const
    {
        const auto* tag = Valid( n ) ? m_Reg->try_get<ECS::TagComponent>( ToEntity( n ) ) : nullptr;
        return tag != nullptr ? std::string_view( tag->Tag ) : std::string_view{};
    }

    std::uint64_t EcsUITree::StableId( NodeId n ) const
    {
        auto* id = Valid( n ) ? m_Reg->try_get<ECS::UUIDComponent>( ToEntity( n ) ) : nullptr;
        return id != nullptr ? static_cast<std::uint64_t>( id->UUID ) : 0u;
    }

    const void* EcsUITree::Find( NodeId n, ArgKind kind ) const
    {
        if ( !Valid( n ) )
            return nullptr;
        const entt::entity e = ToEntity( n );
        return Dispatch( kind,
                         [this, e]<class R>( R ) -> const void*
                         {
                             if constexpr ( std::is_same_v<R, std::nullptr_t> )
                                 return nullptr;
                             else
                             {
                                 const auto* c = m_Reg->try_get<typename R::Component>( e );
                                 return c != nullptr ? &c->Data : nullptr;
                             }
                         } );
    }

    void* EcsUITree::FindState( NodeId n, ArgKind kind )
    {
        if ( !IsStateKind( kind ) || !Valid( n ) )
            return nullptr;
        const entt::entity e = ToEntity( n );
        return Dispatch( kind,
                         [this, e]<class R>( R ) -> void*
                         {
                             if constexpr ( std::is_same_v<R, std::nullptr_t> )
                                 return nullptr;
                             else
                             {
                                 auto* c = m_Reg->try_get<typename R::Component>( e );
                                 return c != nullptr ? &c->Data : nullptr;
                             }
                         } );
    }

    void EcsUITree::Roots( ArgKind kind, std::vector<NodeId>& out ) const
    {
        out.clear();
        std::vector<entt::entity> found;
        Dispatch( kind,
                  [this, &found]<class R>( R )
                  {
                      if constexpr ( !std::is_same_v<R, std::nullptr_t> )
                          for ( const auto e : m_Reg->view<typename R::Component>() )
                              found.push_back( e );
                  } );
        // MEASURED, the view hands its pool out in the REVERSE of creation order (the pool is walked
        // backwards), so authored order is restored here rather than promised by a comment —
        // UICanvasContextPair.CanvasesAreOrderedByTheirAuthoredSortOrder holds it.
        std::sort( found.begin(), found.end(),
                   []( entt::entity a, entt::entity b ) { return CreationIndex( a ) < CreationIndex( b ); } );
        out.reserve( found.size() );
        for ( const auto e : found )
            out.push_back( ToNode( e ) );
    }

    // The canvas entity's own TransformComponent translation — what the world-space billboard has always
    // projected (the local transform: a world-space canvas is authored at the root).
    std::optional<glm::vec3> EcsUITree::WorldOrigin( NodeId n ) const
    {
        const auto* t = Valid( n ) ? m_Reg->try_get<ECS::TransformComponent>( ToEntity( n ) ) : nullptr;
        if ( t == nullptr )
            return std::nullopt;
        return glm::vec3( t->GetTransform()[3] );
    }
} // namespace Desert::UI
