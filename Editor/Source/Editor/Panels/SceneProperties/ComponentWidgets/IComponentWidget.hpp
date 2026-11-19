#pragma once

#include <Engine/ECS/Entity.hpp>
#include <Engine/Core/Scene.hpp>

namespace Desert::Core { class Scene; }

namespace Desert::Editor
{
    class IComponentWidget
    {
    public:
        explicit IComponentWidget( std::string&& widgetName ) : m_WidgetName( std::move( widgetName ) )
        {
        }
        virtual ~IComponentWidget()                                                          = default;
        virtual void               Render( ECS::Entity& entity, ::Desert::Core::Scene* scene = nullptr ) = 0;
        virtual const std::string& GetName() const final
        {
            return m_WidgetName;
        }
        virtual bool CanRemove() const
        {
            return true;
        }

    protected:
        std::string m_WidgetName;
    };

    // `ComponentWidget<ComponentT>` STOOD HERE, AND SO DID THREE PURE VIRTUALS ABOVE:
    // `EntityHasComponent`, `AddComponentToEntity`, `RemoveComponentFromEntity`. They are gone, and the
    // reason is worth stating because the shape reads like a defect and is not one.
    //
    // They were not a generic route that somebody bypassed. They were SUPERSEDED. `ComponentEditorEntry`
    // (Panels/PropertyEditor/ComponentWidgetRegistry.hpp) carries `Has`, `Add` and `Remove` as its own
    // lambdas, and `DrawAddComponentMenu` is the single source of truth for both the Details popup and the
    // scene-outliner context menu. Two mechanisms answered one question; the newer one won, and this half
    // was left standing with nothing calling it — six implementations of three methods, and
    // `RemoveComponentFromEntity`'s body was a COMMENTED-OUT line, which is the tell.
    //
    // Removing them empties the template: `ComponentT` appeared nowhere else in it, so the CRTP layer
    // existed only to write those three bodies. The six widgets now derive from IComponentWidget directly
    // and implement `Render`, which is the one thing a widget is still for — each is constructed inside
    // its own registry entry's Draw lambda (see `TransformComponentWidget().Render( e, s )`).
} // namespace Desert::Editor