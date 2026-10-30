#pragma once

#include <UI/Args/UIArgs.hpp>
#include <UI/Retained/Attribute.hpp>
#include <UI/Retained/Delegate.hpp>
#include <UI/Retained/WidgetTree.hpp>

#include <cstdint>
#include <functional>
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

        // Placement in the parent, as UILayoutData states it.
        WidgetDecl& Anchors( glm::vec2 anchorMin, glm::vec2 anchorMax );
        WidgetDecl& Offsets( glm::vec2 offsetMin, glm::vec2 offsetMax );
        WidgetDecl& Size( glm::vec2 size ); // the preferred size: OffsetMax - OffsetMin

        WidgetDecl& Cursor( UICursorShape cursor );
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
    } // namespace Widgets
} // namespace Desert::UI
