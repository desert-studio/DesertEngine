#include "PropertyEditorBuilder.hpp"
#include "PropertyReset.hpp"
#include "PropertyUndoPolicy.hpp"
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/MultiEdit.hpp>

#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Graphic/ColorTemperature.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Assets/CloudModellingVolumeAsset.hpp>
#include <Engine/Assets/UIThemeAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Font/FontService.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/Image.hpp>

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/EditorPreferences.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>
#include <Editor/Import/TextureDnD.hpp>

#include <Common/Core/Units.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;
    using namespace Desert::Reflection;

    namespace
    {
        void* FieldPtr( void* object, const FieldInfo& f )
        {
            return static_cast<char*>( object ) + f.Offset;
        }

        // Resolves a texture asset handle to its runtime image for a thumbnail preview. Returns a
        // NON-owning shared_ptr (the image is owned by the image service); UICacheTexture keys its ImGui
        // descriptors by VkImageView, so wrapping the same image each frame is safe (no leak).
        std::shared_ptr<Graphic::Image2D> ResolveTextureImage( uint64_t handle )
        {
            if ( handle == 0 )
                return nullptr;
            auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
            if ( !tex )
                return nullptr;
            auto* img = static_cast<Graphic::Image2D*>(
                 Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) );
            if ( !img )
                return nullptr;
            return std::shared_ptr<Graphic::Image2D>( img, []( Graphic::Image2D* ) {} );
        }

        // (texture drop resolution moved to Editor::TextureDnD::ResolveOrImport — import-on-demand)

        // Enums carry any integral underlying type — read/write exactly Size bytes.
        int64_t ReadEnum( const void* p, std::size_t size )
        {
            switch ( size )
            {
                case 1:  return *static_cast<const int8_t*>( p );
                case 2:  return *static_cast<const int16_t*>( p );
                case 8:  return *static_cast<const int64_t*>( p );
                default: return *static_cast<const int32_t*>( p );
            }
        }

        // The unit a field is displayed in, or nullptr when it is a plain number. PROPERTY(Length) is the
        // world-distance case (centimetres — see Common/Core/Units and docs/UNITS.md); PROPERTY(Units("..."))
        // covers everything else. NO value is ever converted: the suffix labels the number as stored.
        const char* UnitSuffix( const Reflection::PropertyMetadata& meta )
        {
            if ( meta.IsLength )
                return "cm";
            return meta.Units.empty() ? nullptr : meta.Units.c_str();
        }

        // How much one pixel of drag should move the value. A centimetre-sized step is right for world
        // distances and useless for a 0..1 ratio, which is the whole reason units are declared.
        float DragSpeedFor( const Reflection::PropertyMetadata& meta )
        {
            const char* unit = UnitSuffix( meta );
            if ( !unit )
                return 0.01f;
            const std::string u( unit );
            if ( u == "cm" || u == "%" )
                return 1.0f;
            if ( u == "deg" )
                return 0.5f;
            if ( u == "s" || u == "ms" )
                return 0.01f;
            return 0.01f;
        }

        // Float widget for a field carrying a unit: same slider/drag as a plain number, with the suffix in
        // the value text and a step that suits the quantity.
        bool DrawUnitScalar( const char* id, float* v, int n, const Reflection::PropertyMetadata& meta )
        {
            char format[32];
            std::snprintf( format, sizeof( format ), "%%.1f %s", UnitSuffix( meta ) );

            const float mn = meta.RangeMin;
            const float mx = meta.RangeMax;
            return meta.HasRange ? ImGui::SliderScalarN( id, ImGuiDataType_Float, v, n, &mn, &mx, format )
                                 : ImGui::DragScalarN( id, ImGuiDataType_Float, v, n, DragSpeedFor( meta ),
                                                       nullptr, nullptr, format );
        }

        // A vector whose components share a declared Range: N sliders bounded by it, exactly as a ranged
        // scalar gets a slider and an unranged one a drag box.
        //
        // The unranged vector keeps the axis-coloured drag boxes, and that asymmetry is the point rather
        // than an oversight. Those colours say "X, Y, Z of a direction in the world", which is true of a
        // position and false of, say, four normalized heights packed into one vector — a field whose
        // components are neither axes nor unbounded. Before this, Range on a vector was accepted by
        // the annotation parser, carried all the way into the generated metadata, and then silently
        // dropped here: the artist got three identical unbounded boxes and no hint of the 0..1 the field
        // actually requires. An annotation that reaches the widget and does nothing is worse than no
        // annotation, because it reads as a guarantee.
        bool DrawRangedVector( const char* id, float* v, int n, const Reflection::PropertyMetadata& meta )
        {
            const float mn = meta.RangeMin;
            const float mx = meta.RangeMax;
            return ImGui::SliderScalarN( id, ImGuiDataType_Float, v, n, &mn, &mx, "%.3f" );
        }

        // Blackbody colour for the temperature slider — the engine's UE-exact Krystek conversion
        // (Graphic::ColorFromTemperature, linear BT.709), normalised so the brightest channel is 1:
        // Kelvin sets the HUE, the light's own Intensity owns brightness. The previous Helland curve-fit
        // produced GAMMA-encoded values, and every colour field here is linear.
        glm::vec3 KelvinToRGB( float kelvin )
        {
            const glm::vec3 c    = Graphic::ColorFromTemperature( kelvin );
            const float     peak = glm::max( c.r, glm::max( c.g, c.b ) );
            return peak > 0.0f ? c / peak : glm::vec3( 1.0f );
        }

        // Rough name for a colour temperature, so the number means something to someone who has never
        // shopped for light bulbs.
        const char* KelvinDescription( float k )
        {
            if ( k < 2200.0f )
                return "candle";
            if ( k < 3200.0f )
                return "warm / tungsten";
            if ( k < 4500.0f )
                return "neutral";
            if ( k < 5500.0f )
                return "cool white";
            if ( k < 7000.0f )
                return "daylight";
            return "overcast / shade";
        }

        // One PROPERTY(Summary) field rendered as text for the component header's one-liner. Returns an
        // empty string for anything that doesn't read well in a single line (a false bool says nothing; a
        // colour is not a word), so the caller can just skip it.
        std::string FormatSummaryValue( const void* object, const FieldInfo& field )
        {
            const void* p    = static_cast<const std::byte*>( object ) + field.Offset;
            const char* unit = UnitSuffix( field.Meta );
            const char* sep  = unit ? " " : "";
            if ( !unit )
                unit = "";

            char buf[128];
            switch ( field.Type )
            {
                case FieldType::Bool:
                    // A true flag names itself ("Cast Shadows"); a false one is not worth the space.
                    return *static_cast<const bool*>( p ) ? field.DisplayName() : std::string();

                case FieldType::Int:
                    std::snprintf( buf, sizeof( buf ), "%d%s%s", *static_cast<const int*>( p ), sep, unit );
                    return buf;

                case FieldType::UInt:
                    std::snprintf( buf, sizeof( buf ), "%u%s%s", *static_cast<const uint32_t*>( p ), sep, unit );
                    return buf;

                case FieldType::Float:
                    std::snprintf( buf, sizeof( buf ), "%.4g%s%s", *static_cast<const float*>( p ), sep, unit );
                    return buf;

                case FieldType::Enum:
                {
                    const int64_t v = ReadEnum( p, field.Size );
                    for ( const auto& ev : field.EnumValues )
                        if ( ev.Value == v )
                            return ev.Name;
                    return {};
                }

                case FieldType::String:
                {
                    const auto& s = *static_cast<const std::string*>( p );
                    return s.size() <= 24 ? s : s.substr( 0, 23 ) + "\xE2\x80\xA6"; // ellipsis
                }

                default:
                    return {};
            }
        }

        // PROPERTY(EditCondition("Foo")) — is the gate open? Looks the named BOOL up in the same block;
        // a leading '!' inverts. An unknown or non-bool name evaluates to TRUE on purpose: a typo in an
        // annotation must not silently freeze a field nobody can then explain.
        bool EditConditionMet( const void* object, const TypeInfo& type, const std::string& condition )
        {
            if ( condition.empty() || !object )
                return true;

            const bool             invert = condition.front() == '!';
            const std::string_view name( condition.data() + ( invert ? 1 : 0 ),
                                         condition.size() - ( invert ? 1 : 0 ) );

            for ( const auto& f : type.Fields )
            {
                if ( f.Name != name || f.Type != FieldType::Bool )
                    continue;
                const bool value =
                     *reinterpret_cast<const bool*>( static_cast<const std::byte*>( object ) + f.Offset );
                return invert ? !value : value;
            }
            return true;
        }

        // --- Details search box ------------------------------------------------------------------
        bool ContainsCI( std::string_view haystack, std::string_view needle )
        {
            if ( needle.empty() )
                return true;
            const auto it = std::search( haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                         []( unsigned char a, unsigned char b )
                                         { return std::tolower( a ) == std::tolower( b ); } );
            return it != haystack.end();
        }

        // A field matches on its label, its C++ name or its category — the three things a user might
        // remember it by.
        bool FieldMatches( const FieldInfo& field, const char* filter )
        {
            if ( !filter || !*filter )
                return true;
            return ContainsCI( field.DisplayName(), filter ) || ContainsCI( field.Name, filter ) ||
                   ContainsCI( field.Meta.Category, filter );
        }

        // --- Category grouping (shared by single- and multi-edit) --------------------------------
        struct CategoryBucket
        {
            std::string                   Name;
            std::vector<const FieldInfo*> Fields;   // in declaration order
            std::vector<const FieldInfo*> Advanced; // PROPERTY(Advanced) — folded at the end
        };

        std::vector<CategoryBucket> GroupFields( const TypeInfo& type, const char* filter )
        {
            std::vector<CategoryBucket> categories;
            auto                        bucket = [&]( const std::string& cat ) -> CategoryBucket&
            {
                for ( auto& c : categories )
                    if ( c.Name == cat )
                        return c;
                categories.push_back( CategoryBucket{ cat, {}, {} } );
                return categories.back();
            };

            for ( const auto& field : type.Fields )
            {
                if ( field.Meta.Hidden || !FieldMatches( field, filter ) )
                    continue;
                auto& c = bucket( field.Meta.Category.empty() ? "Default" : field.Meta.Category );
                ( field.Meta.Advanced ? c.Advanced : c.Fields ).push_back( &field );
            }
            return categories;
        }

        // Draws the grouped categories; `drawRow` submits one field's row so single- and multi-edit keep
        // their own before/after handling.
        bool DrawCategories( const std::vector<CategoryBucket>& categories, bool filtering,
                             const std::function<bool( const FieldInfo& )>& drawRow )
        {
            bool anyChanged = false;

            for ( const auto& cat : categories )
            {
                if ( cat.Fields.empty() && cat.Advanced.empty() )
                    continue;

                // While searching, sections open themselves: hunting for a field and then having to
                // expand the section holding it is exactly what the search box exists to avoid.
                if ( filtering )
                    ImGui::SetNextItemOpen( true, ImGuiCond_Always );
                // The shared section header — a category in a reflected component must look exactly like a
                // section in a hand-written widget, or the panel reads as two different editors.
                if ( !Utils::ImGuiUtilities::SectionHeader( cat.Name.c_str() ) )
                    continue;

                ImGui::Spacing(); // a section, not just another row
                for ( const auto* f : cat.Fields )
                {
                    if ( drawRow( *f ) )
                        anyChanged = true;
                }

                // Rarely-touched fields live behind one fold instead of padding every component.
                if ( !cat.Advanced.empty() )
                {
                    ImGui::PushID( cat.Name.c_str() );
                    if ( filtering )
                        ImGui::SetNextItemOpen( true, ImGuiCond_Always );
                    if ( ImGui::TreeNodeEx( "Advanced", ImGuiTreeNodeFlags_SpanAvailWidth ) )
                    {
                        for ( const auto* f : cat.Advanced )
                        {
                            if ( drawRow( *f ) )
                                anyChanged = true;
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::Spacing();
            }
            return anyChanged;
        }

        void WriteEnum( void* p, std::size_t size, int64_t value )
        {
            switch ( size )
            {
                case 1:  *static_cast<int8_t*>( p )  = static_cast<int8_t>( value ); break;
                case 2:  *static_cast<int16_t*>( p ) = static_cast<int16_t>( value ); break;
                case 8:  *static_cast<int64_t*>( p ) = value; break;
                default: *static_cast<int32_t*>( p ) = static_cast<int32_t>( value ); break;
            }
        }
    } // namespace

    bool PropertyEditorBuilder::DrawField( void* object, const FieldInfo& field,
                                           const Assets::AssetManager* assetMgr, UI::UIHelper* uiHelper,
                                           const void* defaultObject, bool mixed, const TypeInfo* ownerType )
    {
        if ( field.Meta.Hidden )
            return false;

        // PROPERTY(Header("...")) — a labelled section break drawn above this field.
        if ( !field.Meta.Header.empty() )
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled( "%s", field.Meta.Header.c_str() );
        }

        void*       p     = FieldPtr( object, field );
        const auto& label = field.DisplayName();
        bool        changed = false;

        // Kept OUT of `changed` on purpose — see the reset button below. The widget switch assigns to
        // `changed` unconditionally, so anything set before it cannot survive; this is OR-ed back in at
        // the single return.
        bool resetToDefault = false;

        // The default value of THIS field, if the owning type provided a default instance.
        const void* defFieldPtr =
             defaultObject ? static_cast<const std::byte*>( defaultObject ) + field.Offset : nullptr;

        // Nested reflected struct: draw its fields recursively under a collapsible node.
        if ( field.Type == FieldType::Struct )
        {
            ImGui::PushID( field.Name.c_str() );
            if ( field.StructType )
            {
                if ( ImGui::TreeNodeEx( label.c_str(), ImGuiTreeNodeFlags_DefaultOpen ) )
                {
                    for ( const auto& sub : field.StructType->Fields )
                    {
                        if ( DrawField( p, sub, assetMgr, uiHelper, defFieldPtr ) )
                            changed = true;
                    }
                    ImGui::TreePop();
                }
            }
            else
            {
                ImGui::TextDisabled( "%s: (unresolved struct)", label.c_str() );
            }
            ImGui::PopID();
            return changed && !field.Meta.ReadOnly;
        }

        // Capture the field's state BEFORE the widget so an edit can record what to go back to. WHICH
        // fields are recorded, when the entry is pushed and how the state is held is decided once in
        // PropertyUndoPolicy.hpp — this file is drawn by ImGui and reachable by no suite, which is how
        // the previous inline condition managed to exclude every asset slot and include every
        // std::string at the same time.
        const PropertyUndoKind    undoKind = PropertyUndoKindFor( field.Type, field.Meta.ReadOnly, field.Size );
        const PropertyUndoStorage undoStorage =
             PropertyUndoStorageFor( field.Type, field.Meta.ReadOnly, field.Size );

        std::vector<uint8_t> beforeBytes;
        std::string          beforeString;
        if ( undoStorage == PropertyUndoStorage::Bytes )
        {
            beforeBytes.resize( field.Size );
            std::memcpy( beforeBytes.data(), p, field.Size );
        }
        else if ( undoStorage == PropertyUndoStorage::StringValue )
        {
            beforeString = *static_cast<const std::string*>( p );
        }

        // A field whose EditCondition is not met stays visible but inert — the setting exists, it just has
        // no effect in this configuration.
        const bool conditionMet =
             ownerType ? EditConditionMet( object, *ownerType, field.Meta.EditCondition ) : true;

        ImGui::PushID( field.Name.c_str() );

        // "Differs from the type's default" — computed ONCE, here, because two things downstream have to
        // agree about it: the accent tick on the row's left edge (painted by the background below, before
        // any widget) and the reset arrow in the label column (drawn after it). Two independent memcmps
        // would be two chances to disagree, and a row marked dirty with no way to revert it is worse than
        // no mark at all.
        //
        // Byte-level, exactly like ResetFieldToDefault: only trivially-copyable value fields qualify, so
        // the reset that this mark advertises is always the memcpy that PropertyReset performs.
        const bool resettable = defFieldPtr && !field.Meta.ReadOnly && !field.IsContainer &&
                                field.Type != FieldType::String && field.Type != FieldType::Struct &&
                                field.Size > 0;
        // `mixed` counts as modified: with several objects selected the primary may sit at the default
        // while the others do not, and the row IS offering a reset that will change something.
        const bool differsFromDefault = resettable && ( mixed || std::memcmp( p, defFieldPtr, field.Size ) != 0 );

        // Row background (stripe + hover) comes from the shared property-row primitive, so this grid and
        // a hand-written widget cannot drift apart. Painted BEFORE the row: a fill drawn afterwards would
        // cover the widgets.
        const bool rowHovered = Utils::ImGuiUtilities::PropertyRowBackground( 0.0f, differsFromDefault );

        ImGui::Columns( 2 );
        ImGui::SetColumnWidth( 0, Utils::ImGuiUtilities::PropertyLabelWidth() );
        ImGui::AlignTextToFramePadding();
        if ( conditionMet )
            ImGui::TextUnformatted( label.c_str() );
        else
            ImGui::TextDisabled( "%s", label.c_str() ); // greyed like its (disabled) value
        // Multi-select: this field's value differs across the selected objects until the user edits it.
        if ( mixed )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "(mixed)" );
        }
        // Hover the label for help: PROPERTY(Tooltip("...")) when authored, otherwise fall back to
        // revealing the underlying C++ field name (useful when DisplayName is a friendlier alias).
        if ( ImGui::IsItemHovered() )
        {
            if ( !conditionMet )
                ImGui::SetTooltip( "Requires '%s'%s", field.Meta.EditCondition.c_str(),
                                   field.Meta.Tooltip.empty() ? "" : ( "\n" + field.Meta.Tooltip ).c_str() );
            else if ( !field.Meta.Tooltip.empty() )
                ImGui::SetTooltip( "%s", field.Meta.Tooltip.c_str() );
            else if ( field.Name != label )
                ImGui::SetTooltip( "%s", field.Name.c_str() );
        }

        // Pin (favourite): a pinned field is repeated at the TOP of Details, above every component, so the
        // two or three values you actually tune are always in reach. Shown while the row is hovered, and
        // permanently once pinned. Needs a stable identity, so only reflected rows get it.
        float rightEdge = ImGui::GetColumnWidth();
        // Skip the whole thing when there is nothing to draw and nothing to look up — otherwise every
        // row of every component would build a key string each frame just to find an empty list.
        if ( ownerType && ( rowHovered || !EditorPreferences::Get().FavouriteFields.empty() ) )
        {
            const std::string key    = FieldKey( *ownerType, field );
            const bool        pinned = EditorPreferences::IsFavouriteField( key );
            if ( pinned || rowHovered )
            {
                const float bw = ImGui::GetFrameHeight();
                rightEdge -= bw + 2.0f;
                ImGui::SameLine( rightEdge );
                ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
                ImGui::PushStyleColor( ImGuiCol_Text, pinned ? ThemeManager::GetSelectedColor()
                                                             : ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
                if ( ImGui::SmallButton( pinned ? ICON_MDI_STAR : ICON_MDI_STAR_OUTLINE ) )
                    EditorPreferences::ToggleFavouriteField( key );
                ImGui::PopStyleColor( 2 );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( pinned ? "Unpin from the top of Details" : "Pin to the top of Details" );
            }
        }

        // Reset-to-default: for trivially-copyable value fields (not strings/structs/containers) that DIFFER
        // from their default, show a revert button right-aligned in the label column (UE-style). memcpy is
        // safe here because these field types own no heap.
        //
        // Same `differsFromDefault` the accent tick was painted from, so the mark in the margin and the
        // control that clears it appear and disappear together. (Mixed selections are included there for
        // the reason spelled out at its definition: the broadcast below only runs off this row's report.)
        if ( differsFromDefault )
        {
            const float bw = ImGui::GetFrameHeight();
            // Sits left of the pin when one is showing (rightEdge already stepped past it).
            ImGui::SameLine( rightEdge - bw - 2.0f );
            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
            ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetSelectedColor() );
            if ( ImGui::SmallButton( ICON_MDI_BACKUP_RESTORE ) )
            {
                // NOT `changed = true`, and this is the defect this flag used to carry: the widget switch
                // below runs on the SAME frame and every scalar case ASSIGNS
                // (`changed = ImGui::SliderFloat(...)`), which returns false because the click landed on
                // this button — so anything set before the switch cannot survive in `changed`. The flag is
                // OR-ed back in at the single return instead.
                //
                // The reset itself is an EDIT and records itself (undo entry + revision bump) — the
                // widget-commit path below cannot do it, because no widget activates on this click. Before
                // Д29 the memcpy ran silently: the value snapped back on screen while Ctrl+Z, the unsaved-
                // changes star, autosave and the save prompt all learned nothing, so the reset evaporated
                // with the session. `|| mixed`: on a mixed selection the primary may already hold the
                // default (nothing to record here), but the OTHER objects still need the broadcast, and
                // the broadcast only runs when this row reports.
                resetToDefault =
                     ResetFieldToDefault( object, field, defaultObject, CommandHistory::Get() ) || mixed;
            }
            ImGui::PopStyleColor( 2 );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Reset to default" );
        }

        ImGui::NextColumn();
        // Only the VALUE is disabled on a ReadOnly field: the label stays readable and the pin stays
        // clickable (pinning changes the layout, not the value; the reset is gated on !ReadOnly above).
        // An unmet EditCondition disables it for the same reason — the value cannot apply yet.
        ImGui::BeginDisabled( field.Meta.ReadOnly || !conditionMet );
        ImGui::PushItemWidth( -1 );

        switch ( field.Type )
        {
            case FieldType::Bool:
                changed = ImGui::Checkbox( "##v", static_cast<bool*>( p ) );
                break;

            case FieldType::Int:
            {
                int* v = static_cast<int*>( p );
                changed = field.Meta.HasRange
                              ? ImGui::SliderInt( "##v", v, (int)field.Meta.RangeMin, (int)field.Meta.RangeMax )
                              : ImGui::DragInt( "##v", v );
                break;
            }
            case FieldType::UInt:
            {
                changed = ImGui::DragScalar( "##v", ImGuiDataType_U32, p );
                break;
            }
            case FieldType::Float:
            {
                float* v = static_cast<float*>( p );
                if ( UnitSuffix( field.Meta ) )
                {
                    changed = DrawUnitScalar( "##v", v, 1, field.Meta );
                    break;
                }
                changed = field.Meta.HasRange
                              ? ImGui::SliderFloat( "##v", v, field.Meta.RangeMin, field.Meta.RangeMax )
                              : ImGui::DragFloat( "##v", v, 0.01f );
                break;
            }
            case FieldType::Double:
            {
                changed = ImGui::DragScalar( "##v", ImGuiDataType_Double, p, 0.01f );
                break;
            }
            // A vector is drawn with axis-coloured edges (X red / Y green / Z blue), the same colours the
            // viewport gizmo uses — the plain DragFloatN gave three identical boxes you had to count.
            // A COLOUR is not a vector in space and keeps its swatch; a field with a unit keeps the
            // suffixed widget, which carries the unit INSIDE the value text; a field with a declared
            // Range gets sliders bounded by it (DrawRangedVector).
            case FieldType::Vec2:
                changed = UnitSuffix( field.Meta )
                               ? DrawUnitScalar( "##v", static_cast<float*>( p ), 2, field.Meta )
                          : field.Meta.HasRange
                               ? DrawRangedVector( "##v", static_cast<float*>( p ), 2, field.Meta )
                               : Utils::ImGuiUtilities::VectorField( "v", static_cast<float*>( p ), 2, 0.01f );
                break;

            case FieldType::Vec3:
                changed = field.Meta.IsColor ? ImGui::ColorEdit3( "##v", static_cast<float*>( p ) )
                          : UnitSuffix( field.Meta )
                               ? DrawUnitScalar( "##v", static_cast<float*>( p ), 3, field.Meta )
                          : field.Meta.HasRange
                               ? DrawRangedVector( "##v", static_cast<float*>( p ), 3, field.Meta )
                               : Utils::ImGuiUtilities::VectorField( "v", static_cast<float*>( p ), 3, 0.01f );
                break;

            case FieldType::Vec4:
                changed = field.Meta.IsColor ? ImGui::ColorEdit4( "##v", static_cast<float*>( p ) )
                          : UnitSuffix( field.Meta )
                               ? DrawUnitScalar( "##v", static_cast<float*>( p ), 4, field.Meta )
                          : field.Meta.HasRange
                               ? DrawRangedVector( "##v", static_cast<float*>( p ), 4, field.Meta )
                               : Utils::ImGuiUtilities::VectorField( "v", static_cast<float*>( p ), 4, 0.01f );
                break;

            case FieldType::String:
            {
                auto* s = static_cast<std::string*>( p );
                char  buf[256];
                std::snprintf( buf, sizeof( buf ), "%s", s->c_str() );
                if ( ImGui::InputText( "##v", buf, sizeof( buf ) ) )
                {
                    *s      = buf;
                    changed = true;
                }
                break;
            }
            case FieldType::AssetHandle:
            {
                // Font slot: a font is referenced by asset handle (never a raw path). The user picks one of the
                // preloaded fonts from the dropdown or drags a .ttf from the Content Browser. FontService owns
                // the handle<->path registry; "Default" (handle 0) falls back to the engine's built-in font.
                if ( field.Meta.AssetType == "FontAsset" )
                {
                    uint64_t* handle = static_cast<uint64_t*>( p );
                    auto*     fs     = Runtime::ResourceRegistry::GetFontService();

                    const std::string curPath = fs ? fs->PathForHandle( *handle ) : "";
                    const std::string preview =
                         *handle == 0 ? "Default"
                                      : ( curPath.empty() ? "(missing)"
                                                          : std::filesystem::path( curPath ).stem().string() );
                    if ( ImGui::BeginCombo( "##font", preview.c_str() ) )
                    {
                        if ( ImGui::Selectable( "Default", *handle == 0 ) )
                        {
                            *handle = 0;
                            changed = true;
                        }
                        if ( fs )
                        {
                            for ( const auto& f : fs->AvailableFonts() )
                            {
                                const uint64_t h   = fs->RegisterFont( f );
                                const bool     sel = ( h == *handle );
                                if ( ImGui::Selectable( std::filesystem::path( f ).stem().string().c_str(), sel ) )
                                {
                                    *handle = h;
                                    changed = true;
                                }
                                if ( sel )
                                    ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if ( ImGui::BeginDragDropTarget() )
                    {
                        if ( const ImGuiPayload* pl =
                                  ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::FontFile ) )
                        {
                            const std::string path( static_cast<const char*>( pl->Data ),
                                                    pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                            if ( fs && !path.empty() )
                            {
                                *handle = fs->RegisterFont( path );
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Pick a preloaded font or drag a .ttf here from the Content Browser" );
                    break;
                }

                // Icon slot: a vector icon is an .svg imported into an SDF (IconService owns the
                // handle<->path registry). Pick a discovered icon or drag an .svg from the Content Browser —
                // adding icons never touches C++.
                if ( field.Meta.AssetType == "IconAsset" )
                {
                    uint64_t* handle = static_cast<uint64_t*>( p );
                    auto*     is     = Runtime::ResourceRegistry::GetIconService();

                    const std::string curPath = is ? is->PathForHandle( *handle ) : "";
                    const std::string preview =
                         *handle == 0 ? "None"
                                      : ( curPath.empty() ? "(missing)"
                                                          : std::filesystem::path( curPath ).stem().string() );

                    // A REAL preview, not a chip: every colour run stacked at size, each in its own fill, so
                    // a multi-colour icon previews exactly the way it will draw. The atlas alpha channel
                    // carries a sharpened coverage mask (see IconService), hence a crisp silhouette here.
                    constexpr float      kPreview = 96.0f;
                    Runtime::Icon* const icon     = ( is && *handle != 0 ) ? is->Get( *handle ) : nullptr;
                    const bool           drawable = uiHelper && icon && icon->Valid() && is->Atlas();
                    {
                        const ImVec2 at = ImGui::GetCursorScreenPos();
                        const ImVec2 br( at.x + kPreview, at.y + kPreview );
                        ImDrawList*  dl = ImGui::GetWindowDrawList();

                        // Checkerboard backdrop so a light icon reads as clearly as a dark one.
                        dl->AddRectFilled( at, br, IM_COL32( 32, 34, 40, 255 ), 4.0f );
                        dl->PushClipRect( at, br, true );
                        for ( int cy = 0; cy * 12 < static_cast<int>( kPreview ); ++cy )
                            for ( int cx = ( cy & 1 ); cx * 12 < static_cast<int>( kPreview ); cx += 2 )
                                dl->AddRectFilled( ImVec2( at.x + cx * 12.0f, at.y + cy * 12.0f ),
                                                   ImVec2( at.x + cx * 12.0f + 12.0f, at.y + cy * 12.0f + 12.0f ),
                                                   IM_COL32( 44, 47, 55, 255 ) );
                        if ( drawable )
                        {
                            // Fit the source aspect into the box, exactly like the renderer does.
                            const float  side = kPreview - 12.0f;
                            const float  w    = icon->Aspect >= 1.0f ? side : side * icon->Aspect;
                            const float  h    = icon->Aspect >= 1.0f ? side / icon->Aspect : side;
                            const ImVec2 c( at.x + kPreview * 0.5f, at.y + kPreview * 0.5f );
                            const ImVec2 p0( c.x - w * 0.5f, c.y - h * 0.5f );
                            const ImVec2 p1( c.x + w * 0.5f, c.y + h * 0.5f );
                            if ( const void* tex = uiHelper->GetTextureID( is->Atlas() ) )
                                for ( const Runtime::IconLayer& l : icon->Layers )
                                    dl->AddImage( reinterpret_cast<ImTextureID>( const_cast<void*>( tex ) ), p0,
                                                  p1, ImVec2( l.U0, l.V0 ), ImVec2( l.U1, l.V1 ),
                                                  IM_COL32( ( l.RGBA >> 24 ) & 0xFF, ( l.RGBA >> 16 ) & 0xFF,
                                                            ( l.RGBA >> 8 ) & 0xFF, 255 ) );
                        }
                        dl->PopClipRect();
                        dl->AddRect( at, br, IM_COL32( 70, 74, 84, 255 ), 4.0f );
                        ImGui::Dummy( ImVec2( kPreview, kPreview ) );
                    }
                    if ( icon && icon->Valid() )
                        ImGui::TextDisabled( "%zu layer%s", icon->Layers.size(),
                                             icon->Layers.size() == 1 ? "" : "s" );

                    ImGui::SetNextItemWidth( -1.0f );
                    if ( ImGui::BeginCombo( "##icon", preview.c_str() ) )
                    {
                        if ( ImGui::Selectable( "None", *handle == 0 ) )
                        {
                            *handle = 0;
                            changed = true;
                        }
                        if ( is )
                        {
                            for ( const auto& f : is->AvailableIcons() )
                            {
                                const uint64_t h   = is->RegisterIcon( f );
                                const bool     sel = ( h == *handle );
                                if ( ImGui::Selectable( std::filesystem::path( f ).stem().string().c_str(), sel ) )
                                {
                                    *handle = h;
                                    changed = true;
                                }
                                if ( sel )
                                    ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if ( ImGui::BeginDragDropTarget() )
                    {
                        if ( const ImGuiPayload* pl = ImGui::AcceptDragDropPayload( "AssetFile" ) )
                        {
                            const std::string path( static_cast<const char*>( pl->Data ),
                                                    pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                            if ( is && !path.empty() )
                            {
                                *handle = is->RegisterIcon( path );
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Pick a vector icon or drag an .svg here from the Content Browser" );
                    break;
                }

                // Video slot: a video is referenced by asset handle (never a raw path). Drag a .mpg from the
                // Content Browser (a generic AssetFile payload). VideoService owns the handle<->path registry.
                if ( field.Meta.AssetType == "VideoAsset" )
                {
                    uint64_t* handle = static_cast<uint64_t*>( p );
                    auto*     vs     = Runtime::ResourceRegistry::GetVideoService();

                    const std::string curPath = vs ? vs->PathForHandle( *handle ) : "";
                    const std::string display = *handle == 0 ? "None"
                                                : curPath.empty()
                                                     ? "(missing)"
                                                     : std::filesystem::path( curPath ).filename().string();
                    ImGui::Button( display.c_str(), ImVec2( -1.0f, 0.0f ) );
                    if ( ImGui::BeginDragDropTarget() )
                    {
                        if ( const ImGuiPayload* pl = ImGui::AcceptDragDropPayload( "AssetFile" ) )
                        {
                            const std::string path( static_cast<const char*>( pl->Data ),
                                                    pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                            if ( vs && !path.empty() )
                            {
                                *handle = vs->RegisterVideo( path );
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Drag a .mpg (MPEG1) here from the Content Browser" );
                    if ( *handle != 0 )
                    {
                        ImGui::SameLine();
                        if ( ImGui::SmallButton( "x##clearvideo" ) )
                        {
                            *handle = 0;
                            changed = true;
                        }
                    }
                    break;
                }

                // THE CLOUD NOISE VOLUME SLOT USED TO BE HERE, and it went with the field it drew. A
                // `.dcnv` is named by a cloud TYPE now (Engine/Assets/CloudTypeData.hpp) and picked in
                // the Cloud Type panel, so no reflected field carries that asset type and this branch
                // could never be entered — which is the dead path §4.1 says goes with the value it served.

                // THE CLOUD TYPE AND CLOUD LAYOUT SLOTS USED TO BE HERE, and they went with the fields
                // they drew (O1): CloudType1..4 and CloudLayout are MATERIAL schema parameters of
                // CloudRaymarch.shader now, and their rows moved into MaterialEditorPanel::DrawCloudAssetRef
                // — no reflected component field carries either asset type any more, so a branch here
                // could never be entered, which is the dead path §4.1 says goes with the value it served.

                // Cloud modelling volume slot: the sculpted body a hero cloud IS. Its own branch rather
                // than the texture one below for two reasons — there is nothing to thumbnail, and "None"
                // means NO CLOUD here rather than "fall back to a default", which is the opposite of what
                // the cloud type slot above means by an empty entry and would be actively misleading if
                // the two were drawn the same way.
                //
                // Shown by FILE NAME, unlike the type above: a `.dcmv` has no display-name field, because
                // it is a shape rather than a named kind of weather, and the file name is what the artist
                // gave it in the Content Browser.
                if ( field.Meta.AssetType == "UIThemeAsset" )
                {
                    uint64_t* themeHandle = static_cast<uint64_t*>( p );

                    // "None" IS A MEANINGFUL STATE AND SAYS SO. A canvas with no theme is not broken: every
                    // element draws the colours its author typed, which is what every scene authored before
                    // themes existed does. The preview distinguishes that from a handle whose file the asset
                    // scan did not find, because those two look identical on screen.
                    std::string preview = "None (elements use their own colours)";
                    if ( *themeHandle != 0 )
                    {
                        preview = "(missing)";
                        if ( assetMgr )
                        {
                            if ( auto theme = assetMgr->FindByHandle<Assets::UIThemeAsset>(
                                      Common::UUID( *themeHandle ) ) )
                                preview = theme->GetDisplayName();
                        }
                    }

                    ImGui::SetNextItemWidth( -1.0f );
                    if ( ImGui::BeginCombo( "##uitheme", preview.c_str() ) )
                    {
                        if ( ImGui::Selectable( "None (elements use their own colours)", *themeHandle == 0 ) )
                        {
                            *themeHandle = 0;
                            changed      = true;
                        }
                        if ( assetMgr )
                        {
                            for ( const auto& [h, theme] : assetMgr->FindAllByType<Assets::UIThemeAsset>() )
                            {
                                const bool selected = ( static_cast<uint64_t>( h ) == *themeHandle );
                                if ( ImGui::Selectable( theme->GetDisplayName().c_str(), selected ) )
                                {
                                    *themeHandle = static_cast<uint64_t>( h );
                                    changed      = true;
                                }
                                if ( selected )
                                    ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    if ( ImGui::BeginDragDropTarget() )
                    {
                        if ( const ImGuiPayload* pl =
                                  ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::AssetFile ) )
                        {
                            const std::string path( static_cast<const char*>( pl->Data ),
                                                    pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                            // The extension is checked HERE because the Content Browser emits one generic
                            // AssetFile payload for every type it has no icon for. Without the check this
                            // slot would accept a dropped .demat and bind a handle to a file that can never
                            // parse as a theme.
                            if ( assetMgr && !path.empty() &&
                                 std::filesystem::path( path ).extension() == Assets::kUIThemeExtension )
                            {
                                auto& mutableManager = const_cast<Assets::AssetManager&>( *assetMgr );
                                auto  theme          = mutableManager.FindByPath<Assets::UIThemeAsset>( path );
                                if ( !theme )
                                    theme = mutableManager.CreateAsset<Assets::UIThemeAsset>(
                                         Assets::AssetPriority::Medium, path );
                                if ( theme && theme->IsReadyForUse() )
                                {
                                    // Registered on the spot: a theme dropped from outside the shipped
                                    // library is never scanned by the preloader, and without this the
                                    // canvas would hold a handle the service has never heard of — a slot
                                    // that names a theme and draws none.
                                    if ( const auto registered =
                                              Runtime::ResourceRegistry::GetUIThemeService()->Register( theme );
                                         !registered )
                                        LOG_ERROR( "[UI] Dropped theme '{}' could not be registered: {}", path,
                                                   registered.GetError() );

                                    *themeHandle = static_cast<uint64_t>( theme->GetMetadata().Handle );
                                    changed      = true;
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Pick a theme or drag a .detheme here. \"None\" means every "
                                           "element of this canvas draws its own authored colours — which "
                                           "is not a failure, it is how a canvas behaves with no theme." );
                    break;
                }

                if ( field.Meta.AssetType == "CloudModellingVolumeAsset" )
                {
                    uint64_t* volumeHandle = static_cast<uint64_t*>( p );

                    std::string preview = "None (no hero cloud)";
                    if ( *volumeHandle != 0 )
                    {
                        preview = "(missing)";
                        if ( assetMgr )
                        {
                            if ( auto body = assetMgr->FindByHandle<Assets::CloudModellingVolumeAsset>(
                                      Common::UUID( *volumeHandle ) ) )
                                preview = body->GetMetadata().Filepath.filename().string();
                        }
                    }

                    ImGui::SetNextItemWidth( -1.0f );
                    if ( ImGui::BeginCombo( "##cloudbody", preview.c_str() ) )
                    {
                        if ( ImGui::Selectable( "None (no hero cloud)", *volumeHandle == 0 ) )
                        {
                            *volumeHandle = 0;
                            changed       = true;
                        }
                        if ( assetMgr )
                        {
                            for ( const auto& [h, body] :
                                  assetMgr->FindAllByType<Assets::CloudModellingVolumeAsset>() )
                            {
                                const bool selected = ( static_cast<uint64_t>( h ) == *volumeHandle );
                                if ( ImGui::Selectable( body->GetMetadata().Filepath.filename().string().c_str(),
                                                        selected ) )
                                {
                                    *volumeHandle = static_cast<uint64_t>( h );
                                    changed       = true;
                                }
                                if ( selected )
                                    ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    if ( ImGui::BeginDragDropTarget() )
                    {
                        if ( const ImGuiPayload* pl =
                                  ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::AssetFile ) )
                        {
                            const std::string path( static_cast<const char*>( pl->Data ),
                                                    pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                            // The extension is checked HERE because the Content Browser emits one generic
                            // AssetFile payload for every type it has no icon for. Without the check this
                            // slot would accept a dropped .dcnv and bind a handle to a file that can never
                            // parse as a sculpted body.
                            if ( assetMgr && !path.empty() &&
                                 std::filesystem::path( path ).extension() ==
                                      Assets::kCloudModellingVolumeExtension )
                            {
                                auto& mutableManager = const_cast<Assets::AssetManager&>( *assetMgr );
                                auto  body = mutableManager.FindByPath<Assets::CloudModellingVolumeAsset>( path );
                                if ( !body )
                                    body = mutableManager.CreateAsset<Assets::CloudModellingVolumeAsset>(
                                         Assets::AssetPriority::Medium, path );
                                if ( body && body->IsReadyForUse() )
                                {
                                    if ( const auto uploaded =
                                              Runtime::ResourceRegistry::GetCloudModellingService()->Register(
                                                   body );
                                         !uploaded )
                                        LOG_ERROR( "[Clouds] Dropped modelling volume '{}' could not be "
                                                   "uploaded: {}",
                                                   path, uploaded.GetError() );

                                    *volumeHandle = static_cast<uint64_t>( body->GetMetadata().Handle );
                                    changed       = true;
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Pick a sculpted body or drag a .dcmv here. \"None\" means this "
                                           "entity draws no cloud at all — there is no built-in hero cloud, "
                                           "because a body nobody sculpted appearing in a scene is a cloud "
                                           "nobody can explain." );
                    break;
                }

                // Texture slot: shows the bound asset name, accepts a drag-dropped TEXTURE_ASSET (asset
                // path payload, emitted by the FileExplorer) and has a Clear button. Thumbnails are a
                // later visual pass (resolving handle -> Image2D needs runtime verification).
                uint64_t* handle = static_cast<uint64_t*>( p );

                // Always show the texture's filename (consistent); a bound-but-unresolvable handle reads
                // "(missing)", never a raw "Asset #N".
                std::string display = "None";
                if ( *handle != 0 )
                {
                    display = "(missing)";
                    if ( assetMgr )
                    {
                        if ( auto tex = assetMgr->FindByHandle<Assets::TextureAsset>( Common::UUID( *handle ) ) )
                        {
                            const auto& src = tex->GetSourcePath();
                            const auto& path = !src.empty() ? src : tex->GetMetadata().Filepath.string();
                            display          = std::filesystem::path( path ).filename().string();
                        }
                    }
                }

                // PROPERTY(Preview): show the texture INLINE, not just on hover — right for a slot whose
                // content is the point (a sprite, a decal), wrong for a long list of PBR maps. An empty
                // slot still draws its box so the row keeps its shape and reads as "droppable".
                if ( field.Meta.Preview && uiHelper )
                {
                    constexpr float kBox = 72.0f;
                    const ImVec2    at   = ImGui::GetCursorScreenPos();
                    const ImVec2    br( at.x + kBox, at.y + kBox );
                    ImDrawList*     dl = ImGui::GetWindowDrawList();

                    // Checkerboard, so a transparent or light sprite reads as clearly as a dark one.
                    dl->AddRectFilled( at, br, IM_COL32( 32, 34, 40, 255 ), 4.0f );
                    dl->PushClipRect( at, br, true );
                    for ( int cy = 0; cy * 12 < static_cast<int>( kBox ); ++cy )
                        for ( int cx = ( cy & 1 ); cx * 12 < static_cast<int>( kBox ); cx += 2 )
                            dl->AddRectFilled( ImVec2( at.x + cx * 12.0f, at.y + cy * 12.0f ),
                                               ImVec2( at.x + cx * 12.0f + 12.0f, at.y + cy * 12.0f + 12.0f ),
                                               IM_COL32( 44, 47, 55, 255 ) );
                    if ( *handle != 0 )
                    {
                        if ( auto img = ResolveTextureImage( *handle ) )
                        {
                            if ( const void* tex = uiHelper->GetTextureID( img ) )
                                dl->AddImage( reinterpret_cast<ImTextureID>( const_cast<void*>( tex ) ),
                                              ImVec2( at.x + 4.0f, at.y + 4.0f ),
                                              ImVec2( br.x - 4.0f, br.y - 4.0f ) );
                        }
                    }
                    dl->PopClipRect();
                    dl->AddRect( at, br, IM_COL32( 70, 74, 84, 255 ), 4.0f );
                    ImGui::Dummy( ImVec2( kBox, kBox ) );
                }

                ImGui::Button( display.c_str(), ImVec2( -1.0f, 0.0f ) );

                // Hover thumbnail preview of the bound texture.
                if ( uiHelper && *handle != 0 && ImGui::IsItemHovered() )
                {
                    if ( auto img = ResolveTextureImage( *handle ) )
                    {
                        ImGui::BeginTooltip();
                        uiHelper->Image( img, ImVec2( 128.0f, 128.0f ) );
                        ImGui::EndTooltip();
                    }
                }

                if ( ImGui::BeginDragDropTarget() )
                {
                    if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::TextureAsset ) )
                    {
                        const std::string path( static_cast<const char*>( payload->Data ),
                                                payload->DataSize > 0 ? payload->DataSize - 1 : 0 );
                        if ( assetMgr )
                        {
                            // Resolve to a registered texture, importing+cooking on demand if the dropped
                            // source isn't registered yet (so any texture under Resources/ just works).
                            const auto resolved = TextureDnD::ResolveOrImport(
                                 const_cast<Assets::AssetManager&>( *assetMgr ), path );
                            if ( static_cast<uint64_t>( resolved ) != 0 )
                            {
                                *handle = static_cast<uint64_t>( resolved );
                                changed = true;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if ( *handle != 0 )
                {
                    ImGui::SameLine();
                    if ( ImGui::SmallButton( "Clear" ) )
                    {
                        *handle = 0;
                        changed = true;
                    }
                }
                break;
            }

            case FieldType::Enum:
            {
                const auto& values = field.EnumValues;
                if ( values.empty() )
                {
                    ImGui::TextDisabled( "(enum: no values)" );
                    break;
                }

                const int64_t current = ReadEnum( p, field.Size );
                int           idx     = 0;
                for ( size_t k = 0; k < values.size(); ++k )
                    if ( values[k].Value == current )
                        idx = static_cast<int>( k );

                if ( ImGui::BeginCombo( "##v", values[idx].Name.c_str() ) )
                {
                    for ( size_t k = 0; k < values.size(); ++k )
                    {
                        const bool selected = ( static_cast<int>( k ) == idx );
                        if ( ImGui::Selectable( values[k].Name.c_str(), selected ) )
                        {
                            WriteEnum( p, field.Size, values[k].Value );
                            changed = true;
                        }
                        if ( selected )
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                break;
            }

            default:
                ImGui::TextDisabled( "(unsupported)" );
                break;
        }

        // Record an undo command on edit commit. WHEN that happens is decided by PropertyUndoActionFor,
        // not here — an INTERACTIVE row waits for its item to deactivate after an edit, a DISCRETE one
        // (every asset slot, every enum combo) has no such moment because the value was written from
        // inside a popup by a widget that is not this row's item. That difference is why the asset
        // slots were excluded from undo entirely instead of being given the route they needed.
        //
        // The "before" state still has to be carried across frames for the interactive case: ImGui
        // reports the activation on one frame and the commit on a later one. One widget is active at a
        // time, so a single pending capture is enough.
        {
            static void*                s_PendingTarget = nullptr;
            static std::vector<uint8_t> s_PendingOld;
            static std::string          s_PendingOldString;

            if ( undoKind == PropertyUndoKind::Interactive && ImGui::IsItemActivated() )
            {
                s_PendingTarget    = p;
                s_PendingOld       = beforeBytes;
                s_PendingOldString = beforeString;
            }

            const bool pending = ( s_PendingTarget == p );

            PropertyEditSignals signals;
            signals.DeactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit() && pending;
            signals.Changed              = changed;
            signals.ValueDiffers =
                 undoStorage == PropertyUndoStorage::StringValue
                      ? ( *static_cast<const std::string*>( p ) != beforeString )
                      : ( !beforeBytes.empty() && std::memcmp( beforeBytes.data(), p, field.Size ) != 0 );

            // WHERE the "before" state comes from follows the KIND, not the signal: an interactive
            // edit began on an earlier frame and its capture is the pending one, a discrete edit began
            // and ended on this frame and its capture is this row's. Picking by signal instead would
            // read a stale pending buffer on any row that happens to share an address with a finished
            // drag.
            const bool                  interactive = ( undoKind == PropertyUndoKind::Interactive );
            const std::vector<uint8_t>& oldBytes    = interactive ? s_PendingOld : beforeBytes;
            const std::string&          oldString   = interactive ? s_PendingOldString : beforeString;

            switch ( PropertyUndoActionFor( undoKind, undoStorage, signals ) )
            {
                case PropertyUndoAction::PushBytes:
                    if ( oldBytes.size() == field.Size )
                        CommandHistory::Get().Push( p, oldBytes.data(), p, field.Size );
                    s_PendingTarget = nullptr;
                    break;
                case PropertyUndoAction::PushString:
                    CommandHistory::Get().PushString( static_cast<std::string*>( p ), oldString,
                                                      *static_cast<const std::string*>( p ) );
                    s_PendingTarget = nullptr;
                    break;
                case PropertyUndoAction::Nothing:
                    break;
            }
        }

        // PROPERTY(Color, Temperature): a Kelvin slider under the swatch that WRITES the colour. Only the
        // resulting RGB is stored, so the slider keeps its own position for the session (a colour cannot be
        // turned back into one temperature — every neutral grey is 6500 K at some brightness).
        if ( field.Meta.Temperature && field.Meta.IsColor && !field.Meta.ReadOnly &&
             ( field.Type == FieldType::Vec3 || field.Type == FieldType::Vec4 ) )
        {
            static std::unordered_map<const void*, float> s_Kelvin;

            float& kelvin = s_Kelvin.try_emplace( p, 6500.0f ).first->second;
            // UE's own slider range (1700-12000 K); the conversion clamps at 1000-15000.
            if ( ImGui::SliderFloat( "##kelvin", &kelvin, 1700.0f, 12000.0f, "%.0f K" ) )
            {
                const glm::vec3 rgb = KelvinToRGB( kelvin );
                std::memcpy( p, &rgb, sizeof( glm::vec3 ) ); // alpha (Vec4) is deliberately untouched
                changed = true;
            }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Colour temperature — %s.\nWrites the colour above; brightness stays "
                                   "with Intensity.",
                                   KelvinDescription( kelvin ) );

            // Its own undo pair: the block above tracks the colour widget, which is a different item.
            {
                static void*                s_KelvinTarget = nullptr;
                static std::vector<uint8_t> s_KelvinOld;
                if ( ImGui::IsItemActivated() )
                {
                    s_KelvinTarget = p;
                    s_KelvinOld    = beforeBytes;
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() && s_KelvinTarget == p && !s_KelvinOld.empty() )
                {
                    CommandHistory::Get().Push( p, s_KelvinOld.data(), p, field.Size );
                    s_KelvinTarget = nullptr;
                }
            }
        }

        ImGui::PopItemWidth();
        ImGui::EndDisabled();
        ImGui::NextColumn();
        Utils::ImGuiUtilities::PropertyColumnRule();
        ImGui::Columns( 1 );

        ImGui::PopID();

        return ( changed || resetToDefault ) && !field.Meta.ReadOnly;
    }

    bool PropertyEditorBuilder::Draw( void* object, const TypeInfo& type, const Assets::AssetManager* assetMgr,
                                      UI::UIHelper* uiHelper, const char* filter )
    {
        if ( !object )
            return false;

        bool anyChanged = false;

        // Property byte-commands hold raw field pointers, so they must not outlive the object they edit.
        // Drop them when the SELECTED ENTITY changes (several components of one entity draw through here
        // each frame — keying on the object pointer cleared the history every frame). Structural commands
        // are UUID-addressed and survive; Ctrl+Z/Y themselves are handled globally by EditorLayer.
        {
            static uint64_t s_LastSelected = 0;
            const auto&     sel            = Core::SelectionManager::GetSelected();
            const uint64_t  current        = sel.has_value() ? static_cast<uint64_t>( *sel ) : 0;
            if ( current != s_LastSelected )
            {
                CommandHistory::Get().DropVolatile();
                s_LastSelected = current;
            }
        }

        // The type's default-constructed instance (member initializers) — powers reset-to-default.
        const void* defaultObject = type.GetDefaultInstance ? type.GetDefaultInstance() : nullptr;

        Utils::ImGuiUtilities::ResetPropertyRows(); // every component starts its striping alike

        anyChanged = DrawCategories( GroupFields( type, filter ), filter && *filter,
                                     [&]( const FieldInfo& field )
                                     {
                                         return DrawField( object, field, assetMgr, uiHelper, defaultObject,
                                                           /*mixed*/ false, &type );
                                     } );

        return anyChanged;
    }

    bool PropertyEditorBuilder::Draw( void* object, const std::string& typeName,
                                      const Assets::AssetManager* assetMgr, UI::UIHelper* uiHelper,
                                      const char* filter )
    {
        const TypeInfo* type = ReflectionRegistry::Get().Find( typeName );
        if ( !type )
        {
            ImGui::TextDisabled( "<type '%s' not reflected>", typeName.c_str() );
            return false;
        }
        return Draw( object, *type, assetMgr, uiHelper, filter );
    }

    namespace
    {
        // A trivially-copyable value field can be compared/broadcast with memcmp/memcpy across the
        // selection. Strings, nested structs and containers own heap and are excluded (edited on the
        // primary only).
        bool IsBroadcastable( const FieldInfo& field )
        {
            return !field.IsContainer && field.Type != FieldType::Struct &&
                   field.Type != FieldType::String && field.Size > 0 && field.Size <= 64;
        }
    } // namespace

    bool PropertyEditorBuilder::DrawMulti( void* primary, const std::vector<void*>& others, const TypeInfo& type,
                                           const Assets::AssetManager* assetMgr, UI::UIHelper* uiHelper,
                                           const char* filter )
    {
        if ( !primary )
            return false;
        if ( others.empty() )
            return Draw( primary, type, assetMgr, uiHelper, filter );

        Utils::ImGuiUtilities::ResetPropertyRows();

        // The default instance travels into the rows here exactly as in Draw(): the reset button is
        // gated on it, and handing nullptr made the button silently vanish the moment a SECOND object
        // was selected — same panel, same field, different behaviour by selection count (Д29).
        const void* defaultObject = type.GetDefaultInstance ? type.GetDefaultInstance() : nullptr;

        // Same grouping as Draw(), but each field is marked "(mixed)" when it differs across the
        // selection, and a POD edit on the primary is broadcast to every other object.
        return DrawCategories(
             GroupFields( type, filter ), filter && *filter,
             [&]( const FieldInfo& field )
             {
                 const bool broadcastable = IsBroadcastable( field );
                 const bool mixed = broadcastable && AnyFieldDiffers( primary, others, field.Offset, field.Size );

                 const bool changed = DrawField( primary, field, assetMgr, uiHelper, defaultObject, mixed, &type );
                 if ( changed && broadcastable )
                     BroadcastField( primary, others, field.Offset, field.Size );
                 return changed;
             } );
    }

    bool PropertyEditorBuilder::DrawMulti( void* primary, const std::vector<void*>& others,
                                           const std::string& typeName, const Assets::AssetManager* assetMgr,
                                           UI::UIHelper* uiHelper, const char* filter )
    {
        const TypeInfo* type = ReflectionRegistry::Get().Find( typeName );
        if ( !type )
        {
            ImGui::TextDisabled( "<type '%s' not reflected>", typeName.c_str() );
            return false;
        }
        return DrawMulti( primary, others, *type, assetMgr, uiHelper, filter );
    }

    bool PropertyEditorBuilder::MatchesFilter( const TypeInfo& type, const char* filter )
    {
        if ( !filter || !*filter )
            return true;
        for ( const auto& field : type.Fields )
        {
            if ( !field.Meta.Hidden && FieldMatches( field, filter ) )
                return true;
        }
        return false;
    }

    std::string PropertyEditorBuilder::FieldKey( const TypeInfo& type, const FieldInfo& field )
    {
        return type.Name + "." + field.Name;
    }

    std::string PropertyEditorBuilder::BuildSummary( const void* object, const TypeInfo& type )
    {
        if ( !object )
            return {};

        std::string out;
        int         used = 0;
        for ( const auto& field : type.Fields )
        {
            if ( !field.Meta.Summary || field.Meta.Hidden )
                continue;

            const std::string value = FormatSummaryValue( object, field );
            if ( value.empty() )
                continue;

            if ( !out.empty() )
                out += "  \xE2\x80\xA2  "; // bullet
            out += value;

            // Three facts is what fits beside a header before it turns into a second panel.
            if ( ++used == 3 )
                break;
        }
        return out;
    }

    bool PropertyEditorBuilder::DrawPinnedRow( void* object, const TypeInfo& type, const FieldInfo& field,
                                               const Assets::AssetManager* assetMgr, UI::UIHelper* uiHelper )
    {
        const void* defaultObject = type.GetDefaultInstance ? type.GetDefaultInstance() : nullptr;
        return DrawField( object, field, assetMgr, uiHelper, defaultObject, /*mixed*/ false, &type );
    }
} // namespace Desert::Editor
