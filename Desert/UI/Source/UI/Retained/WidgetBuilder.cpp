#include <UI/Retained/WidgetBuilder.hpp>

namespace Desert::UI
{
    WidgetDecl& WidgetDecl::OnClicked( Delegate<void()> onClicked )
    {
        m_Steps.push_back( [onClicked = std::move( onClicked )]( WidgetTree& tree, NodeId n )
                           { tree.OnClicked( n, onClicked ); } );
        return *this;
    }

    WidgetDecl& WidgetDecl::OnDoubleClicked( Delegate<void()> onDoubleClicked )
    {
        m_Steps.push_back( [onDoubleClicked = std::move( onDoubleClicked )]( WidgetTree& tree, NodeId n )
                           { tree.OnDoubleClicked( n, onDoubleClicked ); } );
        return *this;
    }

    WidgetDecl& WidgetDecl::OnValueChanged( Delegate<void( const float& )> onChanged )
    {
        return OnChanged<UISliderData, float>( &UISliderData::Value, std::move( onChanged ) );
    }

    WidgetDecl& WidgetDecl::OnToggled( Delegate<void( const bool& )> onToggled )
    {
        return OnChanged<UIToggleData, bool>( &UIToggleData::Value, std::move( onToggled ) );
    }

    WidgetDecl& WidgetDecl::OnTextChanged( Delegate<void( const std::string& )> onChanged )
    {
        return OnChanged<UIInputFieldData, std::string>( &UIInputFieldData::Text, std::move( onChanged ) );
    }

    WidgetDecl& WidgetDecl::Anchors( glm::vec2 anchorMin, glm::vec2 anchorMax )
    {
        return Edit<UILayoutData>(
             [anchorMin, anchorMax]( UILayoutData& l )
             {
                 l.AnchorMin = anchorMin;
                 l.AnchorMax = anchorMax;
             } );
    }

    WidgetDecl& WidgetDecl::Offsets( glm::vec2 offsetMin, glm::vec2 offsetMax )
    {
        return Edit<UILayoutData>(
             [offsetMin, offsetMax]( UILayoutData& l )
             {
                 l.OffsetMin = offsetMin;
                 l.OffsetMax = offsetMax;
             } );
    }

    WidgetDecl& WidgetDecl::Size( glm::vec2 size )
    {
        return Edit<UILayoutData>( [size]( UILayoutData& l ) { l.OffsetMax = l.OffsetMin + size; } );
    }

    WidgetDecl& WidgetDecl::Cursor( UICursorShape cursor )
    {
        m_Steps.push_back( [cursor]( WidgetTree& tree, NodeId n ) { tree.SetCursor( n, cursor ); } );
        return *this;
    }

    WidgetDecl& WidgetDecl::StableId( std::uint64_t id )
    {
        m_Steps.push_back( [id]( WidgetTree& tree, NodeId n ) { tree.SetStableId( n, id ); } );
        return *this;
    }

    WidgetDecl& WidgetDecl::Expose( NodeId& out )
    {
        m_Expose = &out;
        return *this;
    }

    WidgetDecl& WidgetDecl::operator[]( std::vector<WidgetDecl> children )
    {
        for ( WidgetDecl& c : children )
            m_Children.push_back( std::move( c ) );
        return *this;
    }

    // NOLINTNEXTLINE(misc-no-recursion): a declaration is a finite tree of values, built once.
    NodeId WidgetDecl::Build( WidgetTree& tree, NodeId parent ) const
    {
        const NodeId n = tree.Create( m_Name, parent );
        if ( n == NodeId::Null )
            return NodeId::Null;
        for ( const auto& step : m_Steps )
            step( tree, n );
        for ( const WidgetDecl& c : m_Children )
            c.Build( tree, n );
        if ( m_Expose != nullptr )
            *m_Expose = n;
        return n;
    }

    namespace Widgets
    {
        namespace
        {
            // Fills the parent: a label over its button, a box over its panel.
            WidgetDecl& Fill( WidgetDecl& d )
            {
                return d.Anchors( { 0.0f, 0.0f }, { 1.0f, 1.0f } ).Offsets( { 0.0f, 0.0f }, { 0.0f, 0.0f } );
            }

            WidgetDecl Box( std::string name, UILayoutType type, float spacing )
            {
                UILayoutGroupData g;
                g.Type    = type;
                g.Spacing = spacing;
                WidgetDecl d( std::move( name ) );
                d.With( UILayoutData{} ).With( g );
                return d;
            }
        } // namespace

        WidgetDecl Canvas( std::string name, float referenceWidth, float referenceHeight )
        {
            UICanvasData c;
            c.ReferenceWidth  = referenceWidth;
            c.ReferenceHeight = referenceHeight;
            WidgetDecl d( std::move( name ) );
            d.With( c );
            return d;
        }

        WidgetDecl Panel( std::string name, glm::vec3 color )
        {
            UIPanelData p;
            p.Color = color;
            WidgetDecl d( std::move( name ) );
            d.With( UILayoutData{} ).With( p );
            return d;
        }

        WidgetDecl Text( std::string name, Attribute<std::string> text, float fontSize )
        {
            UITextData t;
            t.FontSize = fontSize;
            WidgetDecl d( std::move( name ) );
            d.With( UILayoutData{} )
                 .With( t )
                 .Bind<UITextData, std::string>( &UITextData::Text, std::move( text ) );
            return d;
        }

        WidgetDecl Button( std::string name, Attribute<std::string> label )
        {
            WidgetDecl labelDecl = Text( name + ".Label", std::move( label ) );
            Fill( labelDecl );
            WidgetDecl d( std::move( name ) );
            d.With( UILayoutData{} ).With( UIButtonData{} )[{ std::move( labelDecl ) }];
            return d;
        }

        WidgetDecl Slider( std::string name, Attribute<float> value, float minValue, float maxValue )
        {
            UISliderData s;
            s.MinValue = minValue;
            s.MaxValue = maxValue;
            WidgetDecl d( std::move( name ) );
            d.With( UILayoutData{} )
                 .With( s )
                 .Bind<UISliderData, float>( &UISliderData::Value, std::move( value ) );
            return d;
        }

        WidgetDecl Toggle( std::string name, Attribute<bool> value )
        {
            WidgetDecl d( std::move( name ) );
            d.With( UILayoutData{} )
                 .With( UIToggleData{} )
                 .Bind<UIToggleData, bool>( &UIToggleData::Value, std::move( value ) );
            return d;
        }

        WidgetDecl InputField( std::string name, Attribute<std::string> text, std::string placeholder )
        {
            UIInputFieldData f;
            if ( !placeholder.empty() )
                f.Placeholder = std::move( placeholder );
            WidgetDecl d( std::move( name ) );
            d.With( UILayoutData{} )
                 .With( std::move( f ) )
                 .Bind<UIInputFieldData, std::string>( &UIInputFieldData::Text, std::move( text ) );
            return d;
        }

        WidgetDecl VerticalBox( std::string name, float spacing )
        {
            return Box( std::move( name ), UILayoutType::Vertical, spacing );
        }

        WidgetDecl HorizontalBox( std::string name, float spacing )
        {
            return Box( std::move( name ), UILayoutType::Horizontal, spacing );
        }

        WidgetDecl ScrollBox( std::string name, float contentHeight )
        {
            UIScrollViewData s;
            s.ContentHeight = contentHeight;
            WidgetDecl d( std::move( name ) );
            d.With( UILayoutData{} ).With( s );
            return d;
        }
    } // namespace Widgets
} // namespace Desert::UI
