#include <Engine/GameplayDebugger/GameplayDebugger.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/GameplayDebugger/GameplayDebuggerCategoryBasic.hpp>
#include <Engine/Input/LocalPlayerInput.hpp>

#include <Common/Core/KeyCodes.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <format>
#include <string>

namespace Desert::Debug
{
    namespace
    {
        // The owner's key (10-16): ` - UE binds ', which sits where a US layout's ` does not.
        constexpr Common::KeyCode kToggleKey = Common::KeyCode::GraveAccent;
        // Digits 1-9 toggle categories 0-8 (UE: the numpad digits).
        constexpr size_t kDigitCategories = 9;

        // The panel's top-left in the view's pixels: clear of the viewport's toolbar row.
        const glm::vec2 kPanelOrigin( 20.0f, 60.0f );

        const glm::vec4 kTitle( 1.0f, 0.85f, 0.2f, 1.0f );
        const glm::vec4 kOn( 0.35f, 1.0f, 0.35f, 1.0f );
        const glm::vec4 kOff( 0.55f, 0.55f, 0.55f, 1.0f );
        const glm::vec4 kText( 1.0f, 1.0f, 1.0f, 1.0f );

        Core::DebugDrawStyle ShapeStyle( const GameplayDebuggerShape& shape )
        {
            return Core::DebugDrawStyle{ .Color = shape.Color };
        }
    } // namespace

    void IGameplayDebuggerCategory::DrawData( Core::Scene& scene, const GameplayDebuggerCategoryData& data,
                                              std::vector<Core::DebugDrawCanvasLine>& panel ) const
    {
        panel.insert( panel.end(), data.Lines.begin(), data.Lines.end() );
        for ( const GameplayDebuggerShape& shape : data.Shapes )
        {
            const Core::DebugDrawStyle style = ShapeStyle( shape );
            switch ( shape.Type )
            {
                case GameplayDebuggerShape::Kind::Point:
                    Core::DebugDraw::Point( scene, shape.A, shape.Radius * 2.0f, style );
                    break;
                case GameplayDebuggerShape::Kind::Segment:
                    Core::DebugDraw::Line( scene, shape.A, shape.B, style );
                    break;
                case GameplayDebuggerShape::Kind::Arrow:
                    Core::DebugDraw::Arrow( scene, shape.A, shape.B, shape.Radius, style );
                    break;
                case GameplayDebuggerShape::Kind::Box:
                    Core::DebugDraw::Box( scene, shape.A, shape.Extent, shape.Rotation, style );
                    break;
                case GameplayDebuggerShape::Kind::Sphere:
                    Core::DebugDraw::Sphere( scene, shape.A, shape.Radius, 12, style );
                    break;
                case GameplayDebuggerShape::Kind::Capsule:
                    Core::DebugDraw::Capsule( scene, shape.A, shape.HalfHeight, shape.Radius, shape.Rotation,
                                              style );
                    break;
            }
            if ( !shape.Description.empty() )
                Core::DebugDraw::String( scene, shape.A, shape.Description, 1.0f, style );
        }
    }

    GameplayDebugger::GameplayDebugger()
    {
        AddCategory( std::make_unique<GameplayDebuggerCategoryBasic>() );
    }

    void GameplayDebugger::AddCategory( std::unique_ptr<IGameplayDebuggerCategory> category )
    {
        const bool enabled = category->IsEnabledByDefault();
        m_Categories.push_back( CategorySlot{ .Category = std::move( category ), .Enabled = enabled } );
    }

    void GameplayDebugger::SetActive( const bool active )
    {
        if ( active == m_Active )
            return;
        m_Active = active;
        // Shown again, every category collects at once: what it held is from before it was hidden.
        for ( CategorySlot& slot : m_Categories )
            slot.Collected = false;
    }

    bool GameplayDebugger::SetCategoryEnabled( const std::string_view name, const bool enabled )
    {
        for ( CategorySlot& slot : m_Categories )
        {
            if ( slot.Category->GetName() != name )
                continue;
            if ( enabled && !slot.Enabled )
                slot.Collected = false;
            slot.Enabled = enabled;
            return true;
        }
        return false;
    }

    bool GameplayDebugger::ToggleCategory( const size_t index )
    {
        if ( index >= m_Categories.size() )
            return false;
        CategorySlot& slot = m_Categories[index];
        return SetCategoryEnabled( slot.Category->GetName(), !slot.Enabled );
    }

    void GameplayDebugger::ReadKeys( const Input::LocalPlayerInput& input )
    {
        if ( input.WasKeyPressed( Input::InputKey::Of( kToggleKey ) ) )
            SetActive( !m_Active );
        if ( !m_Active )
            return;
        for ( size_t i = 0; i < kDigitCategories; ++i )
        {
            const auto digit =
                 static_cast<Common::KeyCode>( static_cast<int>( Common::KeyCode::D1 ) + static_cast<int>( i ) );
            if ( input.WasKeyPressed( Input::InputKey::Of( digit ) ) )
                (void)ToggleCategory( i );
        }
    }

    void GameplayDebugger::PickTarget( const Core::Scene& scene )
    {
        if ( m_Selected && scene.FindEntityByID( *m_Selected ) )
        {
            m_Target = m_Selected;
            return;
        }
        // What the view's centre looks at: the active camera's forward ray.
        if ( const std::shared_ptr<Core::Camera> camera = scene.GetActiveCamera() )
        {
            const glm::mat4         eye = glm::inverse( camera->GetViewMatrix() );
            const Common::Math::Ray ray( glm::vec3( eye[3] ), -glm::vec3( eye[2] ) );
            Core::RaycastHit        hit;
            if ( scene.Raycast( ray, hit ) && hit.Hit )
                m_Target = hit.Entity;
        }
        if ( m_Target && !scene.FindEntityByID( *m_Target ) )
            m_Target.reset(); // destroyed since it was picked
    }

    void GameplayDebugger::Tick( Core::Scene& scene, const Input::LocalPlayerInput* input,
                                 const float deltaSeconds, const bool advancesFrame )
    {
        if ( input != nullptr )
            ReadKeys( *input );
        if ( !m_Active || !advancesFrame )
            return;

        PickTarget( scene );
        const auto         targetRef = m_Target ? scene.FindEntityByID( *m_Target ) : std::nullopt;
        const ECS::Entity* target    = targetRef ? &targetRef->get() : nullptr;
        const bool         retarget  = m_Target != m_CollectedTarget;
        for ( CategorySlot& slot : m_Categories )
        {
            if ( retarget )
                slot.Collected = false;
            if ( !slot.Enabled )
                continue;
            slot.SecondsSinceCollect += deltaSeconds;
            if ( slot.Collected && slot.SecondsSinceCollect < slot.Category->GetCollectInterval() )
                continue;
            slot.Data.Reset();
            slot.Category->CollectData( scene, target, slot.Data );
            slot.Collected           = true;
            slot.SecondsSinceCollect = 0.0f;
        }
        m_CollectedTarget = m_Target;
        Draw( scene );
    }

    void GameplayDebugger::Draw( Core::Scene& scene ) const
    {
        std::vector<Core::DebugDrawCanvasLine> panel;

        std::string targetName = "none";
        if ( m_Target )
            if ( const auto entity = scene.FindEntityByID( *m_Target ) )
                targetName = entity->get().HasComponent<ECS::TagComponent>()
                                  ? entity->get().GetComponent<ECS::TagComponent>().Tag
                                  : std::string( "<untagged>" );
        panel.push_back( { { { "Gameplay Debugger", kTitle },
                             { "   target: ", kOff },
                             { std::move( targetName ), kText },
                             { m_Selected ? "  (selection)" : "  (view centre)", kOff } } } );

        Core::DebugDrawCanvasLine list;
        for ( size_t i = 0; i < m_Categories.size(); ++i )
        {
            const CategorySlot& slot = m_Categories[i];
            list.Runs.push_back(
                 { i < kDigitCategories
                        ? std::format( "{} {} [{}]   ", i + 1, slot.Category->GetName(),
                                       slot.Enabled ? "on" : "off" )
                        : std::format( "{} [{}]   ", slot.Category->GetName(), slot.Enabled ? "on" : "off" ),
                   slot.Enabled ? kOn : kOff } );
        }
        panel.push_back( std::move( list ) );
        panel.push_back( { { { "` hides, 1-9 toggle the categories", kOff } } } );

        for ( const CategorySlot& slot : m_Categories )
        {
            if ( !slot.Enabled || !slot.Collected )
                continue;
            panel.push_back( { { { "", kText } } } );
            panel.push_back( { { { std::string( slot.Category->GetName() ), kTitle } } } );
            slot.Category->DrawData( scene, slot.Data, panel );
        }

        Core::DebugDraw::ScreenPanel( scene, kPanelOrigin, std::move( panel ) );
    }
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS
