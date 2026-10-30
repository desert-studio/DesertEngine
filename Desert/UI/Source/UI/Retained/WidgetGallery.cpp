#include <UI/Retained/WidgetGallery.hpp>

#include <UI/Retained/WidgetBuilder.hpp>

#include <array>
#include <string>
#include <vector>

namespace Desert::UI
{
    NodeId BuildWidgetGallery( WidgetTree& tree, WidgetGalleryModel& model )
    {
        using namespace Widgets;
        WidgetGalleryModel* m = &model;

        // The list: rows a click picks and a double click names.
        constexpr std::array<const char*, 5> kRows = { "Landscape", "Foliage", "Lighting", "Audio", "Gameplay" };
        std::vector<WidgetDecl>               rows;
        for ( const char* row : kRows )
        {
            const std::string label = row;
            rows.push_back( Button( std::string( "Gallery.Row." ) + row, label )
                                 .Size( { 300.0f, 32.0f } )
                                 .OnClicked( [m, label] { m->LastPick = label; } )
                                 .OnDoubleClicked( [m, label] { m->Name = label; } ) );
        }

        return Canvas( "Gallery" )[ {
                    Panel( "Gallery.Background" )
                         .Anchors( { 0.5f, 0.5f }, { 0.5f, 0.5f } )
                         .Offsets( { -340.0f, -300.0f }, { 340.0f, 300.0f } )[ {
                              VerticalBox( "Gallery.Column", 10.0f )
                                   .Anchors( { 0.0f, 0.0f }, { 1.0f, 1.0f } )
                                   .Offsets( { 0.0f, 0.0f }, { 0.0f, 0.0f } )[ {
                                        Text( "Gallery.Title", "Widget gallery (retained tree)", 24.0f )
                                             .Size( { 640.0f, 36.0f } ),
                                        HorizontalBox( "Gallery.ButtonRow", 10.0f )
                                             .Size( { 640.0f, 48.0f } )[ {
                                                  Button( "Gallery.Click", "Click me" )
                                                       .Size( { 200.0f, 48.0f } )
                                                       .OnClicked( [m] { ++m->Clicks; } ),
                                                  Text( "Gallery.ClickCount",
                                                        Attribute<std::string>::Bound(
                                                             [m] { return "Clicks: " + std::to_string( m->Clicks ); } ) )
                                                       .Size( { 200.0f, 48.0f } ),
                                             } ],
                                        Slider( "Gallery.Volume",
                                                Attribute<float>::Bound( [m] { return m->Volume; } ), 0.0f, 1.0f )
                                             .Size( { 640.0f, 24.0f } )
                                             .OnValueChanged( [m]( const float& v ) { m->Volume = v; } ),
                                        Toggle( "Gallery.Enabled", Attribute<bool>::Bound( [m] { return m->Enabled; } ) )
                                             .Size( { 32.0f, 32.0f } )
                                             .OnToggled( [m]( const bool& v ) { m->Enabled = v; } ),
                                        InputField( "Gallery.Name",
                                                    Attribute<std::string>::Bound( [m] { return m->Name; } ),
                                                    "Type a name" )
                                             .Size( { 640.0f, 40.0f } )
                                             .OnTextChanged( [m]( const std::string& v ) { m->Name = v; } ),
                                        Text( "Gallery.Pick",
                                              Attribute<std::string>::Bound( [m] { return "Picked: " + m->LastPick; } ) )
                                             .Size( { 640.0f, 28.0f } ),
                                        ScrollBox( "Gallery.List", 5.0f * 36.0f )
                                             .Size( { 640.0f, 120.0f } )[ {
                                                  VerticalBox( "Gallery.List.Rows", 4.0f )
                                                       .Anchors( { 0.0f, 0.0f }, { 1.0f, 0.0f } )
                                                       .Offsets( { 0.0f, 0.0f }, { 0.0f, 5.0f * 36.0f } )[ rows ],
                                             } ],
                                   } ],
                         } ],
               } ]
             .Build( tree );
    }
} // namespace Desert::UI
