#pragma once

#include <UI/Args/UIArgs.hpp>
#include <UI/Retained/Attribute.hpp>
#include <UI/Retained/Delegate.hpp>
#include <UI/Retained/WidgetTree.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// WIDGETS DECLARED IN C++ — Slate's SNew( SButton ).Text( … ).OnClicked( … )[ … ]. A WidgetDecl is a
// description: a name, the UI*Data arguments the node will carry, the attributes bound into them, the
// delegates it answers with, and its children. Build() turns it into nodes of a WidgetTree; nothing is
// created until then, so a declaration can be composed, returned and stored like any value.
//
//     using namespace Desert::UI::Widgets;
//     const NodeId root = Canvas( "Gallery" )
//         [ {
//             VerticalBox( "Column", 8.0f )
//                 [ {
//                     Button( "Ok", "OK" ).OnClicked( [] { … } ),
//                     Slider( "Volume", Attribute<float>::Bound( [&] { return volume; } ), 0.0f, 1.0f )
//                         .OnValueChanged( [&]( const float& v ) { volume = v; } ),
//                 } ],
//         } ]
//         .Build( tree );
//
// The factories below (Widgets::) are the common shapes; every one is only a WidgetDecl with arguments
// added, and With<T> / Edit<T> / Bind reach any argument the framework has.
namespace Desert::UI
{
    class WidgetDecl
    {
    public:
        explicit WidgetDecl( std::string name ) : m_Name( std::move( name ) )
        {
        }

        // Carry @p data as the node's T::Arg arguments (replacing an earlier With of the same kind).
        template <class T>
        WidgetDecl& With( T data )
        {
            m_Steps.push_back( [data = std::move( data )]( WidgetTree& tree, NodeId n )
                               { tree.Add<T>( n, data ); } );
            return *this;
        }

        // Change the node's T arguments in place — added with their defaults first when absent.
        template <class T, class F>
            requires std::is_invocable_v<F&, T&>
        WidgetDecl& Edit( F fn )
        {
            m_Steps.push_back(
                 [fn = std::move( fn )]( WidgetTree& tree, NodeId n ) mutable
                 {
                     T* data = tree.Edit<T>( n );
                     if ( data == nullptr )
                         data = tree.Add<T>( n );
                     if ( data != nullptr )
                         fn( *data );
                 } );
            return *this;
        }

        // Field @p field of the node's T arguments follows @p value (WidgetTree::Bind).
        template <class T, class V>
        WidgetDecl& Bind( V T::*field, std::type_identity_t<Attribute<V>> value )
        {
            m_Steps.push_back( [field, value = std::move( value )]( WidgetTree& tree, NodeId n )
                               { tree.Bind<T, V>( n, field, value ); } );
            return *this;
        }

        // A change the frame made to field @p field of the node's T arguments (WidgetTree::OnChanged).
        template <class T, class V>
        WidgetDecl& OnChanged( V T::*field, std::type_identity_t<Delegate<void( const V& )>> onChanged )
        {
            m_Steps.push_back( [field, onChanged = std::move( onChanged )]( WidgetTree& tree, NodeId n )
                               { tree.OnChanged<T, V>( n, field, onChanged ); } );
            return *this;
        }

        WidgetDecl& OnClicked( Delegate<void()> onClicked );                    // a button's click
        WidgetDecl& OnDoubleClicked( Delegate<void()> onDoubleClicked );        // a double click inside the node
        WidgetDecl& OnValueChanged( Delegate<void( const float& )> onChanged ); // a slider's drag / step
        WidgetDecl& OnToggled( Delegate<void( const bool& )> onToggled );       // a toggle's flip
        WidgetDecl& OnTextChanged( Delegate<void( const std::string& )> onChanged ); // an input field's edit
        WidgetDecl& OnTextCommitted( Delegate<void( const std::string& )> onCommitted ); // Enter / focus left

        // Run @p fn on the built node, after the steps declared before it (a widget that needs its tree or its
        // id at run time — a spin box asking the tree to focus its text field).
        WidgetDecl& OnBuild( std::function<void( WidgetTree&, NodeId )> fn );

        // Placement in the parent, as UILayoutData states it.
        WidgetDecl& Anchors( glm::vec2 anchorMin, glm::vec2 anchorMax );
        WidgetDecl& Offsets( glm::vec2 offsetMin, glm::vec2 offsetMax );
        WidgetDecl& Size( glm::vec2 size ); // the preferred size: OffsetMax - OffsetMin

        WidgetDecl& Cursor( UICursorShape cursor );
        WidgetDecl& CapturesPointer( bool captures = true ); // a press holds the pointer (a drag handle)

        // The node's own drawing (a paint leaf, WidgetTree::OnPaint): into the frame's draw list, every frame.
        WidgetDecl& OnPaint( Delegate<void( const UIPaintArgs& )> onPaint );
        WidgetDecl& StableId( std::uint64_t id );

        // Write the built node's id to @p out (Slate's SAssignNew).
        WidgetDecl& Expose( NodeId& out );

        // The children, in draw order (Slate's slot operator).
        WidgetDecl& operator[]( std::vector<WidgetDecl> children );

        // Create the node under @p parent (Null = a root), apply every step in declaration order, then build
        // the children. Returns the node, or Null when the tree refused it (WidgetTree::Create).
        NodeId Build( WidgetTree& tree, NodeId parent = NodeId::Null ) const;

    private:
        std::string                                             m_Name;
        std::vector<std::function<void( WidgetTree&, NodeId )>> m_Steps;
        std::vector<WidgetDecl>                                 m_Children;
        NodeId*                                                 m_Expose = nullptr;
    };

    // The common shapes. Each places itself through UILayoutData (the canvas excepted: it is the root).
    enum class SplitAxis : std::uint8_t
    {
        Horizontal, // panes side by side, resized left / right
        Vertical    // panes stacked, resized up / down
    };

    // WHAT A SPLITTER KEEPS — the panes' shares of its length (Slate's SSplitter slot SizeValue), and the box it
    // was last painted in. One layout belongs to ONE splitter (the handles measure the pointer against its
    // box); any number of cells may follow it (SplitCell), which is how a header row's columns and the rows
    // under it stay the same widths (SHeaderRow + its list's rows).
    class SplitLayout
    {
    public:
        // @p ratios are relative (normalised here); empty or non-positive = equal shares. @p minFraction is the
        // smallest share a drag leaves a pane (clamped so every pane can have it).
        SplitLayout( SplitAxis axis, std::vector<float> ratios, float minFraction = 0.05f );

        [[nodiscard]] SplitAxis                 Axis() const;
        [[nodiscard]] std::size_t               Count() const;
        [[nodiscard]] const std::vector<float>& Ratios() const; // sums to 1
        [[nodiscard]] float                     Start( std::size_t pane ) const; // fraction where pane starts
        [[nodiscard]] float                     End( std::size_t pane ) const;

        // Pane @p pane's anchors in the splitter: its span along the axis, the full extent across it.
        [[nodiscard]] glm::vec2 AnchorMin( std::size_t pane ) const;
        [[nodiscard]] glm::vec2 AnchorMax( std::size_t pane ) const;

        // Move the boundary after pane @p boundary to fraction @p t of the length; only the two panes beside
        // it change, and neither drops below the minimum share.
        void MoveBoundary( std::size_t boundary, float t );

        // The splitter's box, noted by its paint every frame; the pointer's fraction along the axis in it.
        void                      NoteBox( const Rect& box );
        [[nodiscard]] const Rect& Box() const;
        [[nodiscard]] float       FractionAt( glm::vec2 pointer ) const;

    private:
        SplitAxis          m_Axis;
        std::vector<float> m_Ratios;
        float              m_MinFraction;
        Rect               m_Box{};
    };

    namespace Widgets
    {
        [[nodiscard]] WidgetDecl Canvas( std::string name, float referenceWidth = 1280.0f,
                                         float referenceHeight = 720.0f );
        [[nodiscard]] WidgetDecl Panel( std::string name, glm::vec3 color = glm::vec3( 0.15f, 0.16f, 0.2f ) );
        [[nodiscard]] WidgetDecl Text( std::string name, Attribute<std::string> text, float fontSize = 18.0f );
        [[nodiscard]] WidgetDecl Button( std::string name, Attribute<std::string> label );
        [[nodiscard]] WidgetDecl Slider( std::string name, Attribute<float> value, float minValue,
                                         float maxValue );
        [[nodiscard]] WidgetDecl Toggle( std::string name, Attribute<bool> value );
        [[nodiscard]] WidgetDecl InputField( std::string name, Attribute<std::string> text,
                                             std::string placeholder = {} );
        [[nodiscard]] WidgetDecl VerticalBox( std::string name, float spacing = 6.0f );
        [[nodiscard]] WidgetDecl HorizontalBox( std::string name, float spacing = 6.0f );
        // A scrolling view of @p contentHeight design px; its rows are its children (a VerticalBox, usually).
        [[nodiscard]] WidgetDecl ScrollBox( std::string name, float contentHeight );

        // A node that draws itself (Slate's SLeafWidget): a rect whose look is @p onPaint.
        [[nodiscard]] WidgetDecl PaintBox( std::string name, Delegate<void( const UIPaintArgs& )> onPaint );

        // SSplitter: @p panes laid along @p layout's axis at its shares, each filling its slot, with a drag
        // handle of @p handlePx between neighbours (resize cursor, pointer capture). The shares live in
        // @p layout — the widget's state — and a drag changes them there.
        [[nodiscard]] WidgetDecl Splitter( std::string name, std::shared_ptr<SplitLayout> layout,
                                           std::vector<WidgetDecl> panes, float handlePx = 6.0f );

        // The slot of pane @p pane of @p layout: a box spanning that pane's share, inset by half of
        // @p handlePx on each side that has a neighbour — exactly where Splitter puts its pane. Put the
        // cell's content inside it ([]). A row's cells under a Columns header are these.
        [[nodiscard]] WidgetDecl SplitCell( std::string name, std::shared_ptr<SplitLayout> layout, std::size_t pane,
                                            float handlePx = 6.0f );

        // SHeaderRow: one labelled header cell per @p labels entry, resizable columns at @p layout's shares
        // (horizontal). Rows below follow the same widths with SplitCell over the same layout.
        [[nodiscard]] WidgetDecl Columns( std::string name, std::shared_ptr<SplitLayout> layout,
                                          std::vector<std::string> labels, float handlePx = 6.0f );

        // SSpinBox: a number dragged with the pointer and typed on a double click. A press holds the pointer
        // (CapturesPointer) and each pixel of horizontal travel moves the value by @p stepPerPx (authored px,
        // so the feel does not change with the canvas scale) from where the drag began, clamped to
        // [@p minValue, @p maxValue]; the fill shows where the value sits in that range. A double click enters
        // text mode: the value becomes an input field holding the keyboard focus, Enter (or focus leaving)
        // commits the parsed number, Escape cancels, and text that is not a number changes nothing. Every
        // change goes out through @p onChanged; the shown value is @p value, read each frame.
        [[nodiscard]] WidgetDecl NumberField( std::string name, Attribute<float> value,
                                              Delegate<void( const float& )> onChanged, float minValue,
                                              float maxValue, float stepPerPx = 0.01f );

        // A colour as UE's details row shows one: a swatch and one NumberField per channel (R, G, B in
        // [0, 1], 1/255 per pixel of drag). A channel's change goes out through @p onChanged as the whole
        // colour, the other channels read from @p value.
        [[nodiscard]] WidgetDecl ColorField( std::string name, Attribute<glm::vec3> value,
                                             Delegate<void( const glm::vec3& )> onChanged );
    } // namespace Widgets
} // namespace Desert::UI
