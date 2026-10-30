#include <UI/Retained/WidgetGallery.hpp>

#include <UI/Retained/WidgetBuilder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Desert::UI
{
    namespace
    {
        // What a "gallery.item" drag carries: the item, and the list it was picked up from.
        struct GalleryItemDrag
        {
            std::size_t From = 0;
            std::string Name;
        };
        constexpr std::string_view kGalleryItemType  = "gallery.item";
        constexpr std::string_view kGalleryColorType = "gallery.color";

        // One list of the drag row: a plate of kDragSlots chips, each showing the item in its slot (hidden past
        // the list's end) and starting a drag of it; the plate takes items from the other list while it has room.
        WidgetDecl GalleryDragList( WidgetGalleryModel* m, std::size_t list, const std::string& name,
                                    glm::vec3 color )
        {
            using namespace Widgets;
            std::vector<WidgetDecl> chips;
            for ( std::size_t slot = 0; slot < WidgetGalleryModel::kDragSlots; ++slot )
            {
                const std::string chip = name + ".Item." + std::to_string( slot );
                WidgetDecl label = Text( chip + ".Label", Attribute<std::string>::Bound(
                                                              [m, list, slot]
                                                              {
                                                                  const auto& items = m->DragLists[list];
                                                                  if ( slot < items.size() )
                                                                      return items[slot];
                                                                  return std::string();
                                                              } ),
                                         16.0f );
                label.Anchors( { 0.0f, 0.0f }, { 1.0f, 1.0f } ).Offsets( { 0.0f, 0.0f }, { 0.0f, 0.0f } );
                std::vector<WidgetDecl> chipChildren;
                chipChildren.push_back( std::move( label ) );

                WidgetDecl item = Panel( chip, glm::vec3( 0.30f, 0.34f, 0.44f ) );
                item.Size( { 64.0f, 32.0f } )
                     .Bind<UILayoutData, UIVisibility>( &UILayoutData::Visibility,
                                                        Attribute<UIVisibility>::Bound(
                                                             [m, list, slot]
                                                             {
                                                                 if ( slot < m->DragLists[list].size() )
                                                                     return UIVisibility::Visible;
                                                                 return UIVisibility::Hidden;
                                                             } ) )
                     .OnDragDetected(
                          [m, list, slot]() -> UIDragPayload
                          {
                              const auto& items = m->DragLists[list];
                              if ( slot >= items.size() )
                                  return {}; // an empty slot carries nothing: no drag
                              UIDragDecorator decorator;
                              decorator.Label = items[slot];
                              return UIDragPayload::Make( std::string( kGalleryItemType ),
                                                          GalleryItemDrag{ list, items[slot] }, decorator );
                          } )[std::move( chipChildren )];
                chips.push_back( std::move( item ) );
            }

            WidgetDecl row = HorizontalBox( name + ".Items", 6.0f );
            row.Anchors( { 0.0f, 0.0f }, { 1.0f, 1.0f } ).Offsets( { 8.0f, 20.0f }, { -8.0f, -20.0f } );
            row[std::move( chips )];
            std::vector<WidgetDecl> plateChildren;
            plateChildren.push_back( std::move( row ) );

            WidgetTree::DropHandler takeItem;
            takeItem.Type    = std::string( kGalleryItemType );
            takeItem.Accepts = [m, list]( const UIDragPayload& p )
            {
                const GalleryItemDrag* drag = p.As<GalleryItemDrag>();
                return drag != nullptr &&
                       ( drag->From == list || m->DragLists[list].size() < WidgetGalleryModel::kDragSlots );
            };
            takeItem.OnDrop = [m, list]( const UIDragPayload& p )
            {
                const GalleryItemDrag* drag = p.As<GalleryItemDrag>();
                if ( drag == nullptr || drag->From == list )
                    return; // dropped back where it came from: nothing moves
                auto&      from = m->DragLists[drag->From];
                const auto it   = std::find( from.begin(), from.end(), drag->Name );
                if ( it == from.end() )
                    return; // the model moved under the drag: the item is no longer there to take
                from.erase( it );
                m->DragLists[list].push_back( drag->Name );
            };

            WidgetDecl plate = Panel( name, color );
            plate.Size( { 220.0f, 72.0f } ).OnDrop( std::move( takeItem ) )[std::move( plateChildren )];
            return plate;
        }

        // The well that refuses items: a drop target for colours only, which writes the colour dropped.
        WidgetDecl GalleryColourWell( WidgetGalleryModel* m )
        {
            using namespace Widgets;
            WidgetDecl label = Text( "Gallery.DragWell.Label", std::string( "Colours only" ), 16.0f );
            label.Anchors( { 0.0f, 0.0f }, { 1.0f, 1.0f } ).Offsets( { 0.0f, 0.0f }, { 0.0f, 0.0f } );
            std::vector<WidgetDecl> children;
            children.push_back( std::move( label ) );

            WidgetTree::DropHandler takeColour;
            takeColour.Type   = std::string( kGalleryColorType );
            takeColour.OnDrop = [m]( const UIDragPayload& p )
            {
                if ( const glm::vec3* colour = p.As<glm::vec3>() )
                    m->Tint = *colour;
            };

            WidgetDecl well = Panel( "Gallery.DragWell", glm::vec3( 0.28f, 0.22f, 0.24f ) );
            well.Size( { 180.0f, 72.0f } ).OnDrop( std::move( takeColour ) )[std::move( children )];
            return well;
        }

        // The paint leaf's own drawing (Widgets::PaintBox): a framed plate, a ring, a triangle and a wave, all
        // from the box it is handed — nothing here is a widget the renderer knows.
        void PaintGalleryShape( const UIPaintArgs& a )
        {
            const glm::vec2 mn( a.Box.X, a.Box.Y );
            const glm::vec2 mx( a.Box.X + a.Box.W, a.Box.Y + a.Box.H );
            const glm::vec2 mid = ( mn + mx ) * 0.5f;
            const float     r   = std::min( a.Box.W, a.Box.H ) * 0.32f;

            a.Dl.AddRectFilled( mn, mx, glm::vec4( 0.10f, 0.11f, 0.14f, 1.0f ), 6.0f * a.Scale );
            a.Dl.AddRect( mn, mx, a.Hot ? glm::vec4( 0.30f, 0.62f, 0.98f, 1.0f ) : glm::vec4( 0.32f, 0.34f, 0.40f, 1.0f ),
                          std::max( 1.0f, a.Scale ) );
            a.Dl.AddRing( { mn.x + a.Box.W * 0.25f, mid.y }, r, r * 0.7f, glm::vec4( 0.95f, 0.62f, 0.20f, 1.0f ),
                          glm::vec4( 0.95f, 0.30f, 0.45f, 1.0f ) );
            a.Dl.AddTriangleFilled( { mid.x, mid.y - r }, { mid.x + r, mid.y + r }, { mid.x - r, mid.y + r },
                                    glm::vec4( 0.35f, 0.80f, 0.55f, 1.0f ) );

            constexpr int                 kWave = 24;
            std::array<glm::vec2, kWave> wave{};
            const float                   x0 = mn.x + a.Box.W * 0.62f;
            const float                   w  = a.Box.W * 0.32f;
            for ( int i = 0; i < kWave; ++i )
            {
                const float t = static_cast<float>( i ) / static_cast<float>( kWave - 1 );
                wave[static_cast<std::size_t>( i )] = { x0 + t * w, mid.y + std::sin( t * 6.2831853f ) * r * 0.8f };
            }
            a.Dl.AddPolyline( wave.data(), static_cast<uint32_t>( wave.size() ), glm::vec4( 0.55f, 0.75f, 1.0f, 1.0f ),
                              2.0f * a.Scale, 1.0f, true );
        }
    } // namespace

    NodeId BuildWidgetGallery( WidgetTree& tree, WidgetGalleryModel& model )
    {
        using namespace Widgets;
        WidgetGalleryModel* m = &model;

        // The columns of the list (SHeaderRow): the header resizes them, every row's cells follow.
        auto columns = std::make_shared<SplitLayout>( SplitAxis::Horizontal, std::vector<float>{ 0.6f, 0.4f } );

        // The list: rows a click picks and a double click names, the kind in a second column.
        constexpr std::array<std::pair<const char*, const char*>, 5> kRows = { {
             { "Landscape", "Terrain" },
             { "Foliage", "Instances" },
             { "Lighting", "Light" },
             { "Audio", "Mixer" },
             { "Gameplay", "Logic" },
        } };
        std::vector<WidgetDecl> rows;
        for ( const auto& [row, kind] : kRows )
        {
            const std::string label  = row;
            const std::string prefix = std::string( "Gallery.Row." ) + row;

            WidgetDecl pick = Button( prefix + ".Pick", label )
                                   .OnClicked( [m, label] { m->LastPick = label; } )
                                   .OnDoubleClicked( [m, label] { m->Name = label; } );
            pick.Anchors( { 0.0f, 0.0f }, { 1.0f, 1.0f } ).Offsets( { 0.0f, 0.0f }, { 0.0f, 0.0f } );
            WidgetDecl kindText = Text( prefix + ".Kind", std::string( kind ), 16.0f );
            kindText.Anchors( { 0.0f, 0.0f }, { 1.0f, 1.0f } ).Offsets( { 0.0f, 0.0f }, { 0.0f, 0.0f } );

            std::vector<WidgetDecl> pickCell;
            pickCell.push_back( std::move( pick ) );
            std::vector<WidgetDecl> kindCell;
            kindCell.push_back( std::move( kindText ) );

            WidgetDecl line( prefix );
            line.With( UILayoutData{} ).Size( { 620.0f, 32.0f } )[ {
                 SplitCell( prefix + ".Cell.0", columns, 0 )[std::move( pickCell )],
                 SplitCell( prefix + ".Cell.1", columns, 1 )[std::move( kindCell )],
            } ];
            rows.push_back( std::move( line ) );
        }

        // The splitter (SSplitter): two panes at 40 / 60, the handle between them drags the share.
        auto split = std::make_shared<SplitLayout>( SplitAxis::Horizontal, std::vector<float>{ 0.4f, 0.6f } );
        std::vector<WidgetDecl> panes;
        {
            WidgetDecl leftText = Text( "Gallery.Split.Left.Label", std::string( "Left" ), 16.0f );
            leftText.Anchors( { 0.0f, 0.0f }, { 1.0f, 1.0f } ).Offsets( { 0.0f, 0.0f }, { 0.0f, 0.0f } );
            WidgetDecl rightText = Text( "Gallery.Split.Right.Label", std::string( "Right" ), 16.0f );
            rightText.Anchors( { 0.0f, 0.0f }, { 1.0f, 1.0f } ).Offsets( { 0.0f, 0.0f }, { 0.0f, 0.0f } );
            std::vector<WidgetDecl> left;
            left.push_back( std::move( leftText ) );
            std::vector<WidgetDecl> right;
            right.push_back( std::move( rightText ) );
            panes.push_back( Panel( "Gallery.Split.Left", glm::vec3( 0.22f, 0.30f, 0.42f ) )[std::move( left )] );
            panes.push_back( Panel( "Gallery.Split.Right", glm::vec3( 0.36f, 0.26f, 0.30f ) )[std::move( right )] );
        }

        return Canvas( "Gallery" )[ {
                    Panel( "Gallery.Background" )
                         .Anchors( { 0.5f, 0.5f }, { 0.5f, 0.5f } )
                         .Offsets( { -340.0f, -300.0f }, { 340.0f, 356.0f } )[ {
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
                                        // The details row: a toggle, a number and a colour, each dragged or
                                        // typed (NumberField / ColorField).
                                        HorizontalBox( "Gallery.ValueRow", 10.0f )
                                             .Size( { 640.0f, 32.0f } )[ {
                                                  Toggle( "Gallery.Enabled",
                                                          Attribute<bool>::Bound( [m] { return m->Enabled; } ) )
                                                       .Size( { 32.0f, 32.0f } )
                                                       .OnToggled( [m]( const bool& v ) { m->Enabled = v; } ),
                                                  NumberField( "Gallery.Speed",
                                                               Attribute<float>::Bound( [m] { return m->Speed; } ),
                                                               [m]( const float& v ) { m->Speed = v; }, 0.0f, 10.0f,
                                                               0.01f )
                                                       .Size( { 160.0f, 32.0f } ),
                                                  ColorField( "Gallery.Tint",
                                                              Attribute<glm::vec3>::Bound( [m] { return m->Tint; } ),
                                                              [m]( const glm::vec3& v ) { m->Tint = v; } )
                                                       .Size( { 360.0f, 32.0f } ),
                                             } ],
                                        InputField( "Gallery.Name",
                                                    Attribute<std::string>::Bound( [m] { return m->Name; } ),
                                                    "Type a name" )
                                             .Size( { 640.0f, 40.0f } )
                                             .OnTextChanged( [m]( const std::string& v ) { m->Name = v; } ),
                                        Text( "Gallery.Pick",
                                              Attribute<std::string>::Bound( [m] { return "Picked: " + m->LastPick; } ) )
                                             .Size( { 640.0f, 28.0f } ),
                                        HorizontalBox( "Gallery.PaintRow", 10.0f )
                                             .Size( { 640.0f, 120.0f } )[ {
                                                  PaintBox( "Gallery.Paint", &PaintGalleryShape )
                                                       .Size( { 200.0f, 120.0f } ),
                                                  Splitter( "Gallery.Split", split, std::move( panes ) )
                                                       .Size( { 430.0f, 120.0f } ),
                                             } ],
                                        Columns( "Gallery.Columns", columns, { "Name", "Kind" } )
                                             .Size( { 620.0f, 28.0f } ),
                                        ScrollBox( "Gallery.List", 5.0f * 36.0f )
                                             .Size( { 640.0f, 120.0f } )[ {
                                                  VerticalBox( "Gallery.List.Rows", 4.0f )
                                                       .Anchors( { 0.0f, 0.0f }, { 1.0f, 0.0f } )
                                                       .Offsets( { 0.0f, 0.0f }, { 0.0f, 5.0f * 36.0f } )[ rows ],
                                             } ],
                                        // The drag-and-drop row (typed payload, UIDragDrop.hpp): list A | list
                                        // B | a well that takes colours only.
                                        HorizontalBox( "Gallery.DragRow", 10.0f )
                                             .Size( { 640.0f, 72.0f } )[ {
                                                  GalleryDragList( m, 0, "Gallery.DragA",
                                                                   glm::vec3( 0.18f, 0.24f, 0.30f ) ),
                                                  GalleryDragList( m, 1, "Gallery.DragB",
                                                                   glm::vec3( 0.20f, 0.28f, 0.22f ) ),
                                                  GalleryColourWell( m ),
                                             } ],
                                   } ],
                         } ],
               } ]
             .Build( tree );
    }
} // namespace Desert::UI
