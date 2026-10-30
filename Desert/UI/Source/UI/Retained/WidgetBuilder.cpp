#include <UI/Retained/WidgetBuilder.hpp>
#include <UI/UICanvasRenderer2D.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <numeric>
#include <string>

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

    WidgetDecl& WidgetDecl::OnTextCommitted( Delegate<void( const std::string& )> onCommitted )
    {
        m_Steps.push_back( [onCommitted = std::move( onCommitted )]( WidgetTree& tree, NodeId n )
                           { tree.OnTextCommitted( n, onCommitted ); } );
        return *this;
    }

    WidgetDecl& WidgetDecl::OnBuild( std::function<void( WidgetTree&, NodeId )> fn )
    {
        m_Steps.push_back( std::move( fn ) );
        return *this;
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

    WidgetDecl& WidgetDecl::CapturesPointer( bool captures )
    {
        m_Steps.push_back( [captures]( WidgetTree& tree, NodeId n ) { tree.SetCapturesPointer( n, captures ); } );
        return *this;
    }

    WidgetDecl& WidgetDecl::OnPaint( Delegate<void( const UIPaintArgs& )> onPaint )
    {
        m_Steps.push_back( [onPaint = std::move( onPaint )]( WidgetTree& tree, NodeId n )
                           { tree.OnPaint( n, onPaint ); } );
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

    // --- SplitLayout ----------------------------------------------------------------------------------------
    SplitLayout::SplitLayout( SplitAxis axis, std::vector<float> ratios, float minFraction )
         : m_Axis( axis ), m_Ratios( std::move( ratios ) )
    {
        if ( m_Ratios.empty() )
            m_Ratios.push_back( 1.0f );
        const bool positive = std::all_of( m_Ratios.begin(), m_Ratios.end(), []( float r ) { return r > 0.0f; } );
        if ( !positive )
            std::fill( m_Ratios.begin(), m_Ratios.end(), 1.0f );
        const float sum = std::accumulate( m_Ratios.begin(), m_Ratios.end(), 0.0f );
        for ( float& r : m_Ratios )
            r /= sum;
        // Every pane must be able to hold the minimum at once.
        m_MinFraction = std::clamp( minFraction, 0.0f, 1.0f / static_cast<float>( m_Ratios.size() ) );
    }

    SplitAxis SplitLayout::Axis() const
    {
        return m_Axis;
    }

    std::size_t SplitLayout::Count() const
    {
        return m_Ratios.size();
    }

    const std::vector<float>& SplitLayout::Ratios() const
    {
        return m_Ratios;
    }

    float SplitLayout::Start( std::size_t pane ) const
    {
        const std::size_t upTo = std::min( pane, m_Ratios.size() );
        return std::accumulate( m_Ratios.begin(), m_Ratios.begin() + static_cast<std::ptrdiff_t>( upTo ), 0.0f );
    }

    float SplitLayout::End( std::size_t pane ) const
    {
        return pane + 1 >= m_Ratios.size() ? 1.0f : Start( pane + 1 );
    }

    glm::vec2 SplitLayout::AnchorMin( std::size_t pane ) const
    {
        return m_Axis == SplitAxis::Horizontal ? glm::vec2( Start( pane ), 0.0f )
                                               : glm::vec2( 0.0f, Start( pane ) );
    }

    glm::vec2 SplitLayout::AnchorMax( std::size_t pane ) const
    {
        return m_Axis == SplitAxis::Horizontal ? glm::vec2( End( pane ), 1.0f ) : glm::vec2( 1.0f, End( pane ) );
    }

    void SplitLayout::MoveBoundary( std::size_t boundary, float t )
    {
        if ( boundary + 1 >= m_Ratios.size() )
            return;
        const float lo         = Start( boundary ) + m_MinFraction;
        const float hi         = End( boundary + 1 ) - m_MinFraction;
        const float at         = std::clamp( t, lo, std::max( lo, hi ) );
        const float pair       = m_Ratios[boundary] + m_Ratios[boundary + 1];
        m_Ratios[boundary]     = at - Start( boundary );
        m_Ratios[boundary + 1] = pair - m_Ratios[boundary];
    }

    void SplitLayout::NoteBox( const Rect& box )
    {
        m_Box = box;
    }

    const Rect& SplitLayout::Box() const
    {
        return m_Box;
    }

    float SplitLayout::FractionAt( glm::vec2 pointer ) const
    {
        if ( m_Axis == SplitAxis::Horizontal )
            return ( pointer.x - m_Box.X ) / std::max( 1.0f, m_Box.W );
        return ( pointer.y - m_Box.Y ) / std::max( 1.0f, m_Box.H );
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

        WidgetDecl PaintBox( std::string name, Delegate<void( const UIPaintArgs& )> onPaint )
        {
            WidgetDecl d( std::move( name ) );
            d.With( UILayoutData{} ).OnPaint( std::move( onPaint ) );
            return d;
        }

        WidgetDecl SplitCell( std::string name, std::shared_ptr<SplitLayout> layout, std::size_t pane,
                              float handlePx )
        {
            const bool      horizontal = layout->Axis() == SplitAxis::Horizontal;
            const float     half       = handlePx * 0.5f;
            const float     lead       = pane > 0 ? half : 0.0f;
            const float     trail      = pane + 1 < layout->Count() ? -half : 0.0f;
            const glm::vec2 insetMin   = horizontal ? glm::vec2( lead, 0.0f ) : glm::vec2( 0.0f, lead );
            const glm::vec2 insetMax   = horizontal ? glm::vec2( trail, 0.0f ) : glm::vec2( 0.0f, trail );

            WidgetDecl d( std::move( name ) );
            d.With( UILayoutData{} )
                 .Offsets( insetMin, insetMax )
                 .Bind<UILayoutData, glm::vec2>(
                      &UILayoutData::AnchorMin,
                      Attribute<glm::vec2>::Bound( [layout, pane] { return layout->AnchorMin( pane ); } ) )
                 .Bind<UILayoutData, glm::vec2>(
                      &UILayoutData::AnchorMax,
                      Attribute<glm::vec2>::Bound( [layout, pane] { return layout->AnchorMax( pane ); } ) );
            return d;
        }

        WidgetDecl Splitter( std::string name, std::shared_ptr<SplitLayout> layout, std::vector<WidgetDecl> panes,
                             float handlePx )
        {
            WidgetDecl d( name );
            // The splitter's own paint notes the box its handles measure the pointer against. It runs before
            // its children are drawn, so a handle reads this frame's box.
            d.With( UILayoutData{} ).OnPaint( [layout]( const UIPaintArgs& a ) { layout->NoteBox( a.Box ); } );

            if ( panes.size() != layout->Count() )
            {
                LOG_ERROR( "[UI] splitter '{}': {} panes for a layout of {} shares; the splitter is built empty",
                           name, panes.size(), layout->Count() );
                return d;
            }

            const bool  horizontal = layout->Axis() == SplitAxis::Horizontal;
            const float half       = handlePx * 0.5f;

            std::vector<WidgetDecl> children;
            for ( std::size_t i = 0; i < panes.size(); ++i )
            {
                // A pane fills its slot, whatever placement it was declared with.
                Fill( panes[i] );
                WidgetDecl cell = SplitCell( name + ".Pane." + std::to_string( i ), layout, i, handlePx );
                std::vector<WidgetDecl> content;
                content.push_back( std::move( panes[i] ) );
                cell[std::move( content )];
                children.push_back( std::move( cell ) );
            }

            // Handles after the panes: drawn over them, and so elected over them where they meet.
            for ( std::size_t k = 0; k + 1 < layout->Count(); ++k )
            {
                const auto edgeMin = [layout, k, horizontal]
                {
                    const float b = layout->End( k );
                    return horizontal ? glm::vec2( b, 0.0f ) : glm::vec2( 0.0f, b );
                };
                const auto edgeMax = [layout, k, horizontal]
                {
                    const float b = layout->End( k );
                    return horizontal ? glm::vec2( b, 1.0f ) : glm::vec2( 1.0f, b );
                };
                const glm::vec2     offsetMin = horizontal ? glm::vec2( -half, 0.0f ) : glm::vec2( 0.0f, -half );
                const glm::vec2     offsetMax = horizontal ? glm::vec2( half, 0.0f ) : glm::vec2( 0.0f, half );
                const UICursorShape cursor =
                     horizontal ? UICursorShape::ResizeLeftRight : UICursorShape::ResizeUpDown;
                WidgetDecl handle( name + ".Handle." + std::to_string( k ) );
                handle.With( UILayoutData{} )
                     .Offsets( offsetMin, offsetMax )
                     .Bind<UILayoutData, glm::vec2>( &UILayoutData::AnchorMin,
                                                     Attribute<glm::vec2>::Bound( edgeMin ) )
                     .Bind<UILayoutData, glm::vec2>( &UILayoutData::AnchorMax,
                                                     Attribute<glm::vec2>::Bound( edgeMax ) )
                     .Cursor( cursor )
                     .CapturesPointer()
                     .OnPaint(
                          [layout, k]( const UIPaintArgs& a )
                          {
                              // SSplitter::OnMouseMove while the handle holds the pointer: the boundary goes to
                              // the pointer's fraction of the splitter. The panes' anchors follow it on the next
                              // RefreshAttributes (one frame later, as every bound value here does).
                              if ( a.Captured && a.HasPointer )
                                  layout->MoveBoundary( k, layout->FractionAt( a.Pointer ) );

                              const glm::vec4 color = a.Captured ? glm::vec4( 0.30f, 0.62f, 0.98f, 1.0f )
                                                      : a.Hot    ? glm::vec4( 0.55f, 0.58f, 0.66f, 1.0f )
                                                                 : glm::vec4( 0.26f, 0.28f, 0.34f, 1.0f );
                              a.Dl.AddRectFilled( { a.Box.X, a.Box.Y }, { a.Box.X + a.Box.W, a.Box.Y + a.Box.H },
                                                  color );
                          } );
                children.push_back( std::move( handle ) );
            }
            d[std::move( children )];
            return d;
        }

        WidgetDecl Columns( std::string name, std::shared_ptr<SplitLayout> layout, std::vector<std::string> labels,
                            float handlePx )
        {
            std::vector<WidgetDecl> cells;
            for ( std::size_t i = 0; i < labels.size(); ++i )
            {
                const std::string cell = name + ".Column." + std::to_string( i );
                WidgetDecl        text = Text( cell + ".Label", labels[i], 16.0f );
                Fill( text );
                std::vector<WidgetDecl> content;
                content.push_back( std::move( text ) );
                WidgetDecl header = Panel( cell, glm::vec3( 0.20f, 0.21f, 0.26f ) );
                header[std::move( content )];
                cells.push_back( std::move( header ) );
            }
            return Splitter( std::move( name ), std::move( layout ), std::move( cells ), handlePx );
        }

        namespace
        {
            // What a NumberField keeps between frames: the drag it is in and the text it is editing. The value
            // itself is the owner's (the attribute); this is only the widget's own state, as SSpinBox keeps
            // its drag anchor and its text-mode flag.
            struct NumberFieldState
            {
                Attribute<float>               Value;
                Delegate<void( const float& )> OnChanged;
                float                          Min  = 0.0f;
                float                          Max  = 1.0f;
                float                          Step = 0.01f;

                WidgetTree* Tree  = nullptr;
                NodeId      Field = NodeId::Null; // the text-mode input field

                bool  Dragging       = false;
                float DragStartX     = 0.0f; // authored px, where the drag began
                float DragStartValue = 0.0f;

                bool        Editing = false;
                std::string Text; // the text-mode field's text
            };

            std::string FormatNumber( float v )
            {
                return std::format( "{:.3f}", v );
            }

            void SetNumber( NumberFieldState& s, float v )
            {
                const float clamped = std::clamp( v, s.Min, s.Max );
                if ( clamped != s.Value.Get() && s.OnChanged.IsBound() )
                    s.OnChanged.Execute( clamped );
            }

            // Leave text mode: the field gives the keyboard focus back (SSpinBox::ExitTextMode).
            void ExitTextMode( NumberFieldState& s )
            {
                s.Editing = false;
                if ( s.Tree != nullptr )
                    s.Tree->RequestKeyboardFocus( NodeId::Null );
            }

            UIVisibility ShownWhen( bool shown, UIVisibility otherwise )
            {
                return shown ? UIVisibility::Visible : otherwise;
            }

            void PaintNumberField( NumberFieldState& s, const UIPaintArgs& a )
            {
                if ( s.Editing )
                {
                    // ESCAPE CANCELS (SSpinBox: the typed text is dropped, the value stays the one before). The
                    // keys are this view's only while it holds the keyboard (UIPaintArgs::Input).
                    s.Dragging = false;
                    if ( a.Input != nullptr && a.Input->Pressed( Common::KeyCode::Escape ) )
                        ExitTextMode( s );
                    return;
                }

                // SSpinBox::OnMouseMove while it holds the pointer: the value moves with the pointer's travel
                // from where the press began, so a drag back to the start gives the start value back exactly.
                const float scale    = std::max( a.Scale, 1e-3f );
                const float pointerX = a.Pointer.x / scale;
                const bool  dragging = a.Captured && a.HasPointer;
                if ( dragging && !s.Dragging )
                {
                    s.DragStartX     = pointerX;
                    s.DragStartValue = s.Value.Get();
                }
                else if ( dragging )
                    SetNumber( s, s.DragStartValue + ( pointerX - s.DragStartX ) * s.Step );
                s.Dragging = dragging;

                // The fill: where the value sits in its range, left to right.
                const float span     = s.Max - s.Min;
                float       fraction = 0.0f;
                if ( span > 0.0f )
                    fraction = std::clamp( ( s.Value.Get() - s.Min ) / span, 0.0f, 1.0f );
                glm::vec4 fill( 0.26f, 0.30f, 0.40f, 0.55f );
                if ( a.Captured )
                    fill = glm::vec4( 0.30f, 0.62f, 0.98f, 0.55f );
                else if ( a.Hot )
                    fill = glm::vec4( 0.36f, 0.42f, 0.55f, 0.55f );
                a.Dl.AddRectFilled( { a.Box.X, a.Box.Y }, { a.Box.X + a.Box.W * fraction, a.Box.Y + a.Box.H },
                                    fill );
            }
        } // namespace

        WidgetDecl NumberField( std::string name, Attribute<float> value, Delegate<void( const float& )> onChanged,
                                float minValue, float maxValue, float stepPerPx )
        {
            auto state       = std::make_shared<NumberFieldState>();
            state->Value     = std::move( value );
            state->OnChanged = std::move( onChanged );
            state->Min       = std::min( minValue, maxValue );
            state->Max       = std::max( minValue, maxValue );
            state->Step      = stepPerPx;

            WidgetDecl shown =
                 Text( name + ".Value",
                       Attribute<std::string>::Bound( [state] { return FormatNumber( state->Value.Get() ); } ),
                       16.0f );
            Fill( shown )
                 .Edit<UILayoutData>( []( UILayoutData& l ) { l.HitTest = UIHitTest::None; } )
                 .Bind<UILayoutData, UIVisibility>(
                      &UILayoutData::Visibility,
                      Attribute<UIVisibility>::Bound( [state]
                                                      { return ShownWhen( !state->Editing, UIVisibility::Hidden ); } ) );

            WidgetDecl typed =
                 InputField( name + ".Edit", Attribute<std::string>::Bound( [state] { return state->Text; } ) );
            Fill( typed )
                 .Bind<UILayoutData, UIVisibility>(
                      &UILayoutData::Visibility,
                      Attribute<UIVisibility>::Bound( [state]
                                                      { return ShownWhen( state->Editing, UIVisibility::Collapsed ); } ) )
                 .OnTextChanged( [state]( const std::string& text ) { state->Text = text; } )
                 .OnTextCommitted(
                      [state]( const std::string& text )
                      {
                          // A commit after Escape (the field losing focus as text mode closed) is not a commit.
                          if ( !state->Editing )
                              return;
                          // strtof, not from_chars: the float overload is missing from the libc++ the Mac
                          // build uses. The whole text must be the number.
                          char*       end    = nullptr;
                          const float parsed = std::strtof( text.c_str(), &end );
                          if ( !text.empty() && end != nullptr && *end == '\0' )
                              SetNumber( *state, parsed );
                          ExitTextMode( *state );
                      } )
                 .Expose( state->Field );

            WidgetDecl d = Panel( std::move( name ), glm::vec3( 0.12f, 0.13f, 0.17f ) );
            d.Cursor( UICursorShape::ResizeLeftRight )
                 .CapturesPointer()
                 .OnBuild( [state]( WidgetTree& tree, NodeId ) { state->Tree = &tree; } )
                 .OnDoubleClicked(
                      [state]
                      {
                          // SSpinBox::EnterTextMode: the text is the value as shown, and the field takes the focus.
                          state->Editing  = true;
                          state->Dragging = false;
                          state->Text     = FormatNumber( state->Value.Get() );
                          if ( state->Tree != nullptr )
                              state->Tree->RequestKeyboardFocus( state->Field );
                      } )
                 .OnPaint( [state]( const UIPaintArgs& a ) { PaintNumberField( *state, a ); } );
            std::vector<WidgetDecl> children;
            children.push_back( std::move( shown ) );
            children.push_back( std::move( typed ) );
            d[std::move( children )];
            return d;
        }

        WidgetDecl ColorField( std::string name, Attribute<glm::vec3> value,
                               Delegate<void( const glm::vec3& )> onChanged )
        {
            WidgetDecl              d = HorizontalBox( name, 6.0f );
            std::vector<WidgetDecl> parts;
            WidgetDecl              swatch = PaintBox( name + ".Swatch",
                                                       [value]( const UIPaintArgs& a )
                                                       {
                                              a.Dl.AddRectFilled( { a.Box.X, a.Box.Y },
                                                                  { a.Box.X + a.Box.W, a.Box.Y + a.Box.H },
                                                                  glm::vec4( value.Get(), 1.0f ) );
                                          } );
            swatch.Size( { 32.0f, 32.0f } );
            parts.push_back( std::move( swatch ) );
            constexpr std::array<const char*, 3> kChannels{ ".R", ".G", ".B" };
            for ( int c = 0; c < 3; ++c )
            {
                WidgetDecl channel = NumberField(
                     name + kChannels[static_cast<std::size_t>( c )],
                     Attribute<float>::Bound( [value, c] { return value.Get()[c]; } ),
                     [value, onChanged, c]( const float& v )
                     {
                         glm::vec3 colour = value.Get();
                         colour[c]        = v;
                         if ( onChanged.IsBound() )
                             onChanged.Execute( colour );
                     },
                     0.0f, 1.0f, 1.0f / 255.0f );
                channel.Size( { 100.0f, 32.0f } );
                parts.push_back( std::move( channel ) );
            }
            d[std::move( parts )];
            return d;
        }
    } // namespace Widgets
} // namespace Desert::UI
