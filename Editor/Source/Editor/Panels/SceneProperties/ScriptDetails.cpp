#include <Editor/Panels/SceneProperties/ScriptDetails.hpp>

#include <Editor/Core/AssetPickerRows.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/EntityPickRequest.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserUtils.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Editor/Widgets/AssetFieldOpen.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>
#include <Engine/Scripting/ScriptSchema.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        namespace fs = std::filesystem;
        using Scripting::PropertyType;
        using Scripting::ScriptProperty;
        using Scripting::ScriptPropertyDecl;
        using Scripting::ScriptSchema;
        using Scripting::ScriptStatus;
        using Scripting::ScriptValue;
        using Utils::ImGuiUtilities;

        // ---- The scripts a slot can name ---------------------------------------------------------------

        // The scripts of the PROJECT (Constants::Path::SCRIPT_PATH, loose and mounted alike), as the
        // root-tagged KEYS a slot stores (ECS::ScriptSlot::ScriptKey): minted here, at the one place the picker
        // turns a file into a reference, so no stored value can be a rooted path. Sorted: a dropdown that
        // reshuffles between sessions is one nobody can learn.
        std::vector<std::string> ProjectScriptKeys()
        {
            std::vector<std::string> scripts;
            for ( const fs::path& file :
                  Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::SCRIPT_PATH ) )
                if ( file.extension() == ".lua" )
                    scripts.push_back( Common::AssetHandle::StableKeyForPath( file ) );
            std::sort( scripts.begin(), scripts.end() );
            return scripts;
        }

        // What to CALL a script: its path under the scripts root, unique where the file name is not.
        std::string ScriptLabel( const std::string& scriptKey )
        {
            const fs::path    file = Common::AssetHandle::PathForStableKey( scriptKey );
            std::error_code   ec;
            const std::string relative =
                 fs::relative( file, Common::Constants::Path::SCRIPT_PATH, ec ).generic_string();
            return ( ec || relative.empty() ) ? file.filename().generic_string() : relative;
        }

        // ---- The schema, read once per file version ------------------------------------------------------

        // Reading a schema compiles the script and runs its top level in a throwaway VM: far too much for every
        // frame. Re-read when the file's write time moves (looked at twice a second), so an edit saved in the
        // code editor reaches Details without a button.
        struct CachedSchema
        {
            bool               Read = false;
            fs::file_time_type Stamp{};
            double             CheckedAt = 0.0;
            ScriptSchema       Schema;
        };

        std::unordered_map<std::string, CachedSchema>& SchemaCache()
        {
            static std::unordered_map<std::string, CachedSchema> cache;
            return cache;
        }

        const ScriptSchema& SchemaOf( const fs::path& file )
        {
            CachedSchema& entry = SchemaCache()[file.generic_string()];
            const double  now   = ImGui::GetTime();
            if ( entry.Read && now - entry.CheckedAt < 0.5 )
                return entry.Schema;
            entry.CheckedAt = now;
            std::error_code          ec;
            const fs::file_time_type stamp = fs::last_write_time( file, ec );
            if ( !entry.Read || ( !ec && stamp != entry.Stamp ) )
            {
                entry.Schema = Scripting::ReadScriptSchema( file.generic_string() );
                entry.Stamp  = ec ? fs::file_time_type{} : stamp;
                entry.Read   = true;
            }
            return entry.Schema;
        }

        // ---- Values: the slot holds overrides only -------------------------------------------------------

        bool Fits( const ScriptProperty& value, const ScriptPropertyDecl& decl )
        {
            return value.Type == decl.Default.Type &&
                   ( value.Type != PropertyType::Array || value.ElementType == decl.ElementType );
        }

        ScriptProperty* OverrideOf( ECS::ScriptSlot& slot, const std::string& name )
        {
            const auto found = std::find_if( slot.Properties.begin(), slot.Properties.end(),
                                             [&]( const ScriptProperty& p ) { return p.Name == name; } );
            return found == slot.Properties.end() ? nullptr : &*found;
        }

        // What the property holds on this entity: its override when it has one of the declared kind, else the
        // script's default.
        ScriptProperty EffectiveValue( ECS::ScriptSlot& slot, const ScriptPropertyDecl& decl )
        {
            if ( const ScriptProperty* held = OverrideOf( slot, decl.Name );
                 held != nullptr && Fits( *held, decl ) )
                return *held;
            return decl.Default;
        }

        // Stores `value` as the entity's value of `decl`: an override while it differs from the default, nothing
        // once it is the default again (so a later default in the script reaches this entity too).
        void StoreValue( ECS::ScriptSlot& slot, const ScriptPropertyDecl& decl, ScriptProperty value )
        {
            value.Name = decl.Name;
            std::erase_if( slot.Properties, [&]( const ScriptProperty& p ) { return p.Name == decl.Name; } );
            if ( !( value == decl.Default ) )
                slot.Properties.push_back( std::move( value ) );
        }

        // ---- Undo ----------------------------------------------------------------------------------------

        struct EditTarget
        {
            Common::UUID Entity;
            bool         Undoable = true; // false in a played world: its edits are thrown away with it
        };

        // A one-shot edit (a combo pick, a button): one undo step around it.
        void Apply( const EditTarget& target, const std::function<void()>& edit )
        {
            if ( target.Undoable )
                Commands::MutateEntityUndoable( target.Entity, edit );
            else
                edit();
        }

        // A continuous edit (a drag, a slider, typing): ONE undo step from the press to the release. The state
        // to go back to is the one BEFORE the widget ran this frame — ImGui changes the value on the very frame
        // it activates — so the caller hands the properties as they were, and they are swapped in for the
        // snapshot.
        std::shared_ptr<const Commands::EntityStateSnapshot>& PendingSnapshot()
        {
            static std::shared_ptr<const Commands::EntityStateSnapshot> pending;
            return pending;
        }

        void TrackContinuous( const EditTarget& target, ECS::ScriptSlot& slot,
                              std::vector<ScriptProperty>& before )
        {
            if ( !target.Undoable )
                return;
            if ( ImGui::IsItemActivated() )
            {
                std::swap( slot.Properties, before );
                PendingSnapshot() = Commands::CaptureEntityState( target.Entity );
                std::swap( slot.Properties, before );
            }
            if ( ImGui::IsItemDeactivated() )
            {
                if ( ImGui::IsItemDeactivatedAfterEdit() && PendingSnapshot() )
                    Commands::CommitEntityState( PendingSnapshot() );
                PendingSnapshot().reset();
            }
        }

        // ---- Row furniture (UE5 Details) -----------------------------------------------------------------

        // Opens a property row the way PropertyEditorBuilder::DrawField does — the shared row background with
        // the accent tick of a modified value, the label column (greyed while an EditCondition is unmet, its
        // tooltip on hover), the reset arrow — and leaves the value column open for ONE widget. True when the
        // reset arrow was clicked.
        bool BeginRow( const char* label, const std::string& tooltip, bool modified, bool enabled,
                       float height = 0.0f )
        {
            ImGuiUtilities::PropertyRowBackground( height, modified );
            ImGui::Columns( 2, nullptr, false );
            ImGui::SetColumnWidth( 0, ImGuiUtilities::PropertyLabelWidth() );
            ImGui::AlignTextToFramePadding();
            if ( enabled )
                ImGui::TextUnformatted( label );
            else
                ImGui::TextDisabled( "%s", label );
            if ( !tooltip.empty() && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
                ImGui::SetTooltip( "%s", tooltip.c_str() );

            bool reset = false;
            if ( modified )
            {
                const float button = ImGui::GetFrameHeight();
                ImGui::SameLine( ImGui::GetColumnWidth() - button - 2.0f );
                ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
                ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetSelectedColor() );
                reset = ImGui::SmallButton( ICON_MDI_BACKUP_RESTORE "##reset" );
                ImGui::PopStyleColor( 2 );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Reset to the script's default" );
            }
            ImGui::NextColumn();
            ImGui::BeginDisabled( !enabled );
            ImGui::PushItemWidth( -1.0f );
            return reset;
        }

        void EndRow()
        {
            ImGui::PopItemWidth();
            ImGui::EndDisabled();
            ImGui::NextColumn();
            ImGuiUtilities::PropertyColumnRule();
            ImGui::Columns( 1 );
        }

        // A small flat icon button, the kind UE strings beside a slot.
        bool IconButton( const char* icon, const char* tooltip, bool active = false )
        {
            ImGui::PushStyleColor( ImGuiCol_Button, active ? ThemeManager::GetSelectedColor()
                                                           : ImGui::GetStyleColorVec4( ImGuiCol_FrameBg ) );
            const bool clicked = ImGui::Button( icon, ImVec2( ImGui::GetFrameHeight(), ImGui::GetFrameHeight() ) );
            ImGui::PopStyleColor();
            if ( tooltip != nullptr && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
                ImGui::SetTooltip( "%s", tooltip );
            return clicked;
        }

        // A category of a script's properties: a lighter, indented fold under the slot's bar.
        bool CategoryHeader( const std::string& name )
        {
            const ImVec4 bar = ThemeManager::GetSectionHeaderColor();
            ImGui::PushStyleColor( ImGuiCol_Header, ImVec4( bar.x * 0.85f, bar.y * 0.85f, bar.z * 0.85f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_HeaderHovered,
                                   ImVec4( bar.x + 0.04f, bar.y + 0.04f, bar.z + 0.04f, 1.0f ) );
            ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 6.0f, 3.0f ) );
            ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 0.0f );
            const bool open = ImGui::TreeNodeEx(
                 name.c_str(), ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth |
                                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen );
            ImGui::PopStyleVar( 2 );
            ImGui::PopStyleColor( 2 );
            ImGuiUtilities::ResetPropertyRows();
            return open;
        }

        // ---- Asset slots -----------------------------------------------------------------------------------

        std::optional<Common::Content::ContentKind> KindNamed( const std::string& name )
        {
            for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
            {
                const auto kind = static_cast<Common::Content::ContentKind>( i );
                if ( Common::Content::KindName( kind ) == name )
                    return kind;
            }
            return std::nullopt;
        }

        // The picture of an asset slot's content. A mesh is photographed by the shared service — ASKED here,
        // never only read from the disk cache (Tests/Editor/ThumbnailRequesters) — every other kind shows its
        // glyph: the frame is there either way, so the slot keeps its shape.
        void AssetThumbnailOf( Common::Content::ContentKind kind, const Assets::ContentRegistry::PickerRow* row,
                               float size, const ComponentEditContext& context )
        {
            static ThumbnailCache s_Thumbnails;

            std::shared_ptr<Graphic::Image2D> picture;
            if ( row != nullptr && kind == Common::Content::ContentKind::StaticMesh )
            {
                const std::string source = row->Path.generic_string();
                const std::string png    = ThumbnailService::Get().RequestMesh( row->Handle, source );
                if ( !png.empty() )
                {
                    if ( ThumbnailService::JudgeMeshPicture( source ) == ThumbnailFreshness::Verdict::Show )
                        picture = s_Thumbnails.Get( png );
                    else
                        s_Thumbnails.Invalidate( png );
                }
            }

            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 br( at.x + size, at.y + size );
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            ImGui::Dummy( ImVec2( size, size ) );
            dl->AddRectFilled( at, br, IM_COL32( 15, 15, 15, 255 ), 2.0f );
            if ( picture && context.UIHelper != nullptr )
            {
                if ( const void* texture = context.UIHelper->GetTextureID( picture ) )
                    dl->AddImageRounded( reinterpret_cast<ImTextureID>( const_cast<void*>( texture ) ),
                                         ImVec2( at.x + 1.0f, at.y + 1.0f ), ImVec2( br.x - 1.0f, br.y - 1.0f ),
                                         ImVec2( 0, 0 ), ImVec2( 1, 1 ), IM_COL32_WHITE, 2.0f );
            }
            else
            {
                const char*  icon = row == nullptr ? ICON_MDI_FILE_QUESTION_OUTLINE
                                    : kind == Common::Content::ContentKind::Texture ? ICON_MDI_IMAGE_OUTLINE
                                                                                    : ICON_MDI_CUBE_OUTLINE;
                const ImVec2 ts   = ImGui::CalcTextSize( icon );
                dl->AddText( ImVec2( at.x + ( size - ts.x ) * 0.5f, at.y + ( size - ts.y ) * 0.5f ),
                             ImGui::GetColorU32( ImGuiCol_TextDisabled ), icon );
            }
            // The asset-type bar under the preview, as UE draws it.
            dl->AddRectFilled( ImVec2( at.x, br.y - 3.0f ), br,
                               ImGui::GetColorU32( ThemeManager::GetSelectedColor() ) );
            dl->AddRect( at, br, ImGui::GetColorU32( ImGuiCol_Border ), 2.0f );
        }

        // An Asset value: [picture] [sunk slot with the name ▾] over a strip of [open] [browse] [clear]. Picks
        // from the content registry's rows of the declared kind, or a file dropped from the Content Browser.
        bool AssetField( ScriptValue& value, const Reflection::PropertyMetadata& meta,
                         const ComponentEditContext& context )
        {
            const std::optional<Common::Content::ContentKind> kind = KindNamed( meta.AssetType );
            if ( !kind )
            {
                ImGui::TextColored( ThemeManager::GetErrorColor(), "unknown asset kind '%s'",
                                    meta.AssetType.c_str() );
                return false;
            }
            std::optional<Assets::ContentRegistry::PickerRow> row;
            if ( value.Ref != 0 )
                row = Assets::ContentRegistry::RowOf( *kind, value.Ref );

            bool        changed = false;
            const float picture = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
            AssetThumbnailOf( *kind, row ? &*row : nullptr, picture, context );
            ImGui::SameLine();
            ImGui::BeginGroup();
            const std::string name = value.Ref == 0 ? std::format( "None ({})", meta.AssetType )
                                     : row          ? PickerDisplayName( *row )
                                                    : std::string( "(missing)" );
            if ( ImGuiUtilities::AssetSlot( "slot", name.c_str(), value.Ref == 0 ) )
                ImGui::OpenPopup( "pick" );
            if ( ImGui::BeginDragDropTarget() )
            {
                if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( DragPayloads::AssetFile ) )
                {
                    const std::string path( static_cast<const char*>( payload->Data ),
                                            payload->DataSize > 0 ? payload->DataSize - 1 : 0 );
                    if ( const auto dropped = Assets::ContentRegistry::RowAtPath( *kind, path ) )
                    {
                        value.Ref = static_cast<std::uint64_t>( dropped->Handle );
                        changed   = true;
                    }
                    else
                        ToastManager::Push( std::format( "'{}' is no {} asset",
                                                         fs::path( path ).filename().string(), meta.AssetType ),
                                            ToastLevel::Warning, 3.0f );
                }
                ImGui::EndDragDropTarget();
            }
            if ( ImGui::BeginPopup( "pick" ) )
            {
                static ImGuiTextFilter filter;
                filter.Draw( ICON_MDI_MAGNIFY "##filter", 260.0f );
                if ( ImGui::Selectable( "None", value.Ref == 0 ) )
                {
                    value.Ref = 0;
                    changed   = true;
                }
                ImGui::BeginChild( "rows", ImVec2( 300.0f, 260.0f ) );
                for ( const auto& candidate : Assets::ContentRegistry::Rows( *kind ) )
                {
                    const std::string label = PickerDisplayName( candidate );
                    if ( !filter.PassFilter( label.c_str() ) )
                        continue;
                    const auto handle = static_cast<std::uint64_t>( candidate.Handle );
                    if ( ImGui::Selectable( std::format( "{}##{}", label, handle ).c_str(), handle == value.Ref ) )
                    {
                        value.Ref = handle;
                        changed   = true;
                        ImGui::CloseCurrentPopup();
                    }
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "%s", candidate.Key.c_str() );
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
            DrawAssetFieldButtons( value.Ref );
            ImGui::SameLine();
            ImGui::BeginDisabled( value.Ref == 0 );
            if ( IconButton( ICON_MDI_CLOSE, "Clear" ) )
            {
                value.Ref = 0;
                changed   = true;
            }
            ImGui::EndDisabled();
            ImGui::EndGroup();
            return changed;
        }

        // ---- Entity slots ----------------------------------------------------------------------------------

        std::string EntityName( ::Desert::Core::Scene* scene, std::uint64_t uuid )
        {
            if ( uuid == 0 )
                return "None";
            if ( scene == nullptr )
                return "(no world)";
            const auto found = scene->FindEntityByID( Common::UUID( uuid ) );
            if ( !found )
                return "(missing)";
            const ECS::Entity& entity = found->get();
            return entity.HasComponent<ECS::TagComponent>() ? entity.GetComponent<ECS::TagComponent>().Tag
                                                            : std::string( "(unnamed)" );
        }

        // An Entity value: [⊙ name ▾] [eyedropper] [select] [clear]. Picks from the world's entities, from an
        // Outliner row dropped on it, or from a click in the viewport while the eyedropper is armed.
        bool EntityField( ScriptValue& value, ::Desert::Core::Scene* scene )
        {
            bool              changed = false;
            const std::string name    = EntityName( scene, value.Ref );
            const ImGuiID     owner   = ImGui::GetID( "eyedropper" );

            if ( const std::optional<Common::UUID> picked = EntityPickRequest::TakeFor( owner ) )
            {
                value.Ref = static_cast<std::uint64_t>( *picked );
                changed   = true;
            }

            const float buttons = ( ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x ) * 3.0f;
            ImGui::BeginGroup();
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.0f, 0.5f ) );
            ImGui::PushStyleColor( ImGuiCol_Button, ImGui::GetStyleColorVec4( ImGuiCol_FrameBg ) );
            ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( value.Ref == 0 ? ImGuiCol_TextDisabled
                                                                                           : ImGuiCol_Text ) );
            if ( ImGui::Button( std::format( "{}  {}##entity", ICON_MDI_TARGET, name ).c_str(),
                                ImVec2( ImGui::GetContentRegionAvail().x - buttons, 0.0f ) ) )
                ImGui::OpenPopup( "pick" );
            ImGui::PopStyleColor( 2 );
            ImGui::PopStyleVar();
            if ( ImGui::BeginDragDropTarget() )
            {
                if ( const ImGuiPayload* payload =
                          ImGui::AcceptDragDropPayload( DragPayloads::EntityRelationship );
                     payload != nullptr && payload->DataSize == sizeof( Common::UUID ) )
                {
                    Common::UUID dropped;
                    std::memcpy( &dropped, payload->Data, sizeof( Common::UUID ) );
                    value.Ref = static_cast<std::uint64_t>( dropped );
                    changed   = true;
                }
                ImGui::EndDragDropTarget();
            }
            if ( ImGui::BeginPopup( "pick" ) )
            {
                static ImGuiTextFilter filter;
                filter.Draw( ICON_MDI_MAGNIFY "##filter", 260.0f );
                if ( ImGui::Selectable( "None", value.Ref == 0 ) )
                {
                    value.Ref = 0;
                    changed   = true;
                }
                ImGui::BeginChild( "rows", ImVec2( 300.0f, 260.0f ) );
                if ( scene != nullptr )
                {
                    auto& registry = scene->GetRegistry();
                    for ( const entt::entity handle : registry.view<ECS::UUIDComponent, ECS::TagComponent>() )
                    {
                        const std::string& tag = registry.get<ECS::TagComponent>( handle ).Tag;
                        const auto         uuid =
                             static_cast<std::uint64_t>( registry.get<ECS::UUIDComponent>( handle ).UUID );
                        if ( !filter.PassFilter( tag.c_str() ) )
                            continue;
                        if ( ImGui::Selectable( std::format( "{}##{}", tag, uuid ).c_str(), uuid == value.Ref ) )
                        {
                            value.Ref = uuid;
                            changed   = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            const bool armed = EntityPickRequest::IsArmedFor( owner );
            ImGui::PushID( "eyedropper" );
            if ( IconButton( ICON_MDI_EYEDROPPER,
                             armed ? "Click an entity in the viewport (Esc cancels)"
                                   : "Pick an entity in the viewport",
                             armed ) )
            {
                if ( armed )
                    EntityPickRequest::Cancel();
                else
                    EntityPickRequest::Arm( owner );
            }
            ImGui::PopID();
            if ( armed && ImGui::IsKeyPressed( ImGuiKey_Escape ) )
                EntityPickRequest::Cancel();
            ImGui::SameLine();
            ImGui::BeginDisabled( value.Ref == 0 || name.starts_with( "(" ) );
            if ( IconButton( ICON_MDI_CROSSHAIRS_GPS, "Select it" ) )
                Core::SelectionManager::SetSelected( Common::UUID( value.Ref ) );
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled( value.Ref == 0 );
            if ( IconButton( ICON_MDI_CLOSE, "Clear" ) )
            {
                value.Ref = 0;
                changed   = true;
            }
            ImGui::EndDisabled();
            ImGui::EndGroup();
            return changed;
        }

        // ---- One value of any scalar kind --------------------------------------------------------------------

        std::string Format( const char* number, const Reflection::PropertyMetadata& meta )
        {
            return meta.Units.empty() ? std::string( number ) : std::format( "{} {}", number, meta.Units );
        }

        bool ValueWidget( const char* id, ScriptValue& value, const Reflection::PropertyMetadata& meta,
                          const std::vector<std::string>& options, ::Desert::Core::Scene* scene,
                          const ComponentEditContext& context )
        {
            ImGui::PushID( id );
            bool changed = false;
            switch ( value.Type )
            {
                case PropertyType::Number:
                {
                    auto              v      = static_cast<float>( value.Number );
                    const std::string format = Format( "%.3f", meta );
                    changed                  = meta.HasRange
                                                    ? ImGui::SliderFloat( "##v", &v, meta.RangeMin, meta.RangeMax, format.c_str() )
                                                    : ImGui::DragFloat( "##v", &v, 0.05f, 0.0f, 0.0f, format.c_str() );
                    if ( changed )
                        value.Number = v;
                    break;
                }
                case PropertyType::Int:
                {
                    auto              v      = static_cast<int>( value.Number );
                    const std::string format = Format( "%d", meta );
                    changed = meta.HasRange ? ImGui::SliderInt( "##v", &v, static_cast<int>( meta.RangeMin ),
                                                                static_cast<int>( meta.RangeMax ), format.c_str() )
                                            : ImGui::DragInt( "##v", &v, 0.2f, 0, 0, format.c_str() );
                    if ( changed )
                        value.Number = v;
                    break;
                }
                case PropertyType::Bool:
                    changed = ImGui::Checkbox( "##v", &value.Bool );
                    break;
                case PropertyType::String:
                {
                    char buffer[512] = {};
                    std::strncpy( buffer, value.Str.c_str(), sizeof( buffer ) - 1 );
                    changed = ImGui::InputText( "##v", buffer, sizeof( buffer ) );
                    if ( changed )
                        value.Str = buffer;
                    break;
                }
                case PropertyType::Color:
                {
                    float c[3] = { static_cast<float>( value.Vec[0] ), static_cast<float>( value.Vec[1] ),
                                   static_cast<float>( value.Vec[2] ) };
                    changed    = ImGui::ColorEdit3( "##v", c, ImGuiColorEditFlags_Float );
                    if ( changed )
                        value.Vec = { c[0], c[1], c[2] };
                    break;
                }
                case PropertyType::Vector:
                {
                    float v[3] = { static_cast<float>( value.Vec[0] ), static_cast<float>( value.Vec[1] ),
                                   static_cast<float>( value.Vec[2] ) };
                    changed    = meta.HasRange ? ImGui::SliderFloat3( "##v", v, meta.RangeMin, meta.RangeMax )
                                               : ImGuiUtilities::VectorField( "v", v, 3, 1.0f, "%.1f" );
                    if ( changed )
                        value.Vec = { v[0], v[1], v[2] };
                    break;
                }
                case PropertyType::Enum:
                    if ( ImGui::BeginCombo( "##v", value.Str.c_str() ) )
                    {
                        for ( const std::string& option : options )
                            if ( ImGui::Selectable( option.c_str(), option == value.Str ) )
                            {
                                value.Str = option;
                                changed   = true;
                            }
                        ImGui::EndCombo();
                    }
                    break;
                case PropertyType::Asset:
                    changed = AssetField( value, meta, context );
                    break;
                case PropertyType::Entity:
                    changed = EntityField( value, scene );
                    break;
                case PropertyType::Array:
                    break; // drawn by ArrayRows
            }
            ImGui::PopID();
            return changed;
        }

        // Whether the kind's widget is a CONTINUOUS edit (one undo step from press to release) rather than a
        // one-shot pick.
        bool Continuous( PropertyType type )
        {
            return type == PropertyType::Number || type == PropertyType::Int || type == PropertyType::String ||
                   type == PropertyType::Color || type == PropertyType::Vector;
        }

        float RowHeightOf( PropertyType type )
        {
            return type == PropertyType::Asset ? ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y +
                                                      ImGui::GetStyle().CellPadding.y * 2.0f
                                               : 0.0f;
        }

        // ---- The slot ----------------------------------------------------------------------------------------

        struct SlotView
        {
            ECS::ScriptSlot&            Slot;
            const ScriptSchema&         Schema;
            const EditTarget&           Target;
            ::Desert::Core::Scene*                Scene;
            const ComponentEditContext& Context;
        };

        bool ConditionMet( SlotView& view, const ScriptPropertyDecl& decl )
        {
            if ( decl.Meta.EditCondition.empty() )
                return true;
            const bool        invert = decl.Meta.EditCondition.starts_with( '!' );
            const std::string gate   = invert ? decl.Meta.EditCondition.substr( 1 ) : decl.Meta.EditCondition;
            const ScriptPropertyDecl* gateDecl = Scripting::FindDecl( view.Schema, gate );
            if ( gateDecl == nullptr )
                return true; // ReadScriptSchema refuses this; nothing to grey by
            return EffectiveValue( view.Slot, *gateDecl ).Bool != invert;
        }

        std::string TooltipOf( const ScriptPropertyDecl& decl, bool conditionMet )
        {
            std::string tip = decl.Meta.Tooltip.empty() ? decl.Name : decl.Meta.Tooltip;
            if ( !conditionMet )
                tip = std::format( "Requires '{}'\n{}", decl.Meta.EditCondition, tip );
            return std::format( "{}\n\n{} · {}", tip, decl.Name,
                                Scripting::PropertyTypeName( decl.Default.Type ) );
        }

        // An Array: a header row (count, +, clear) and a row per item ([≡ i] value −), the items reorderable by
        // dragging their index.
        void ArrayRows( SlotView& view, const ScriptPropertyDecl& decl, const std::string& label, bool enabled )
        {
            ScriptProperty value    = EffectiveValue( view.Slot, decl );
            const bool     modified = !( value == decl.Default );
            bool           changed  = false;

            const bool reset = BeginRow( label.c_str(), TooltipOf( decl, enabled ), modified, enabled );
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled( "%zu %s", value.Items.size(), value.Items.size() == 1 ? "item" : "items" );
            ImGui::SameLine( ImGui::GetContentRegionMax().x - ( ImGui::GetFrameHeight() + 4.0f ) * 2.0f );
            if ( IconButton( ICON_MDI_PLUS, "Add an item" ) )
            {
                ScriptValue item = decl.Default.Items.empty() ? ScriptValue{} : decl.Default.Items.back();
                item.Type        = decl.ElementType;
                if ( decl.ElementType == PropertyType::Enum && !decl.EnumOptions.empty() && item.Str.empty() )
                    item.Str = decl.EnumOptions.front();
                if ( decl.ElementType == PropertyType::Color && decl.Default.Items.empty() )
                    item.Vec = { 1.0, 1.0, 1.0 };
                value.Items.push_back( item );
                changed = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled( value.Items.empty() );
            if ( IconButton( ICON_MDI_DELETE_OUTLINE, "Remove every item" ) )
            {
                value.Items.clear();
                changed = true;
            }
            ImGui::EndDisabled();
            EndRow();

            // One row per item: [=] value [-]. The handle is a drag source and a drop target: drop an item's
            // handle on another's to move it there (only within this array: the payload names it).
            struct ItemDrag
            {
                ImGuiID     Array = 0;
                std::size_t Index = 0;
            };
            const ImGuiID                                      arrayId = ImGui::GetID( "items" );
            std::optional<std::size_t>                         remove;
            std::optional<std::pair<std::size_t, std::size_t>> move;
            for ( std::size_t i = 0; i < value.Items.size(); ++i )
            {
                ImGui::PushID( static_cast<int>( i ) );
                const std::string index = std::format( "    [{}]", i );
                BeginRow( index.c_str(), {}, false, enabled, RowHeightOf( decl.ElementType ) );
                ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
                ImGui::Button( ICON_MDI_DRAG_VERTICAL "##drag" );
                ImGui::PopStyleColor();
                if ( ImGui::IsItemHovered() )
                    ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
                if ( ImGui::BeginDragDropSource() )
                {
                    const ItemDrag payload{ arrayId, i };
                    ImGui::SetDragDropPayload( "SCRIPT_ARRAY_ITEM", &payload, sizeof( payload ) );
                    ImGui::Text( "Move item %zu", i );
                    ImGui::EndDragDropSource();
                }
                if ( ImGui::BeginDragDropTarget() )
                {
                    if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( "SCRIPT_ARRAY_ITEM" );
                         payload != nullptr && payload->DataSize == sizeof( ItemDrag ) )
                    {
                        ItemDrag dragged;
                        std::memcpy( &dragged, payload->Data, sizeof( dragged ) );
                        if ( dragged.Array == arrayId )
                            move = std::make_pair( dragged.Index, i );
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine();
                std::vector<ScriptProperty> before = view.Slot.Properties;
                ScriptValue&                item   = value.Items[i];
                const float                 minus  = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
                ImGui::PushItemWidth( ImGui::GetContentRegionAvail().x - minus );
                const bool itemChanged =
                     ValueWidget( "item", item, decl.ElementMeta, decl.EnumOptions, view.Scene, view.Context );
                ImGui::PopItemWidth();
                if ( Continuous( decl.ElementType ) )
                    TrackContinuous( view.Target, view.Slot, before );
                changed = changed || itemChanged;
                ImGui::SameLine();
                if ( IconButton( ICON_MDI_MINUS, "Remove this item" ) )
                    remove = i;
                EndRow();
                ImGui::PopID();
            }
            if ( remove )
            {
                value.Items.erase( value.Items.begin() + static_cast<std::ptrdiff_t>( *remove ) );
                changed = true;
            }
            if ( move && move->first != move->second )
            {
                ScriptValue item = value.Items[move->first];
                value.Items.erase( value.Items.begin() + static_cast<std::ptrdiff_t>( move->first ) );
                value.Items.insert( value.Items.begin() + static_cast<std::ptrdiff_t>( move->second ), item );
                changed = true;
            }

            if ( reset )
                Apply( view.Target, [&] { StoreValue( view.Slot, decl, decl.Default ); } );
            else if ( changed )
            {
                // A drag inside an item is recorded by TrackContinuous; the structural edits are one step each.
                if ( ImGui::IsAnyItemActive() )
                    StoreValue( view.Slot, decl, value );
                else
                    Apply( view.Target, [&] { StoreValue( view.Slot, decl, value ); } );
            }
        }

        void PropertyRow( SlotView& view, const ScriptPropertyDecl& decl )
        {
            ImGui::PushID( decl.Name.c_str() );
            const bool        enabled = ConditionMet( view, decl );
            const std::string label   = decl.Meta.DisplayName.empty() ? decl.Name : decl.Meta.DisplayName;
            if ( decl.Default.Type == PropertyType::Array )
            {
                ArrayRows( view, decl, label, enabled );
                ImGui::PopID();
                return;
            }

            ScriptProperty value    = EffectiveValue( view.Slot, decl );
            const bool     modified = !( value == decl.Default );
            const bool     reset    = BeginRow( label.c_str(), TooltipOf( decl, enabled ), modified, enabled,
                                                RowHeightOf( decl.Default.Type ) );
            std::vector<ScriptProperty> before = view.Slot.Properties;
            const bool                  changed =
                 ValueWidget( "value", value, decl.Meta, decl.EnumOptions, view.Scene, view.Context );
            if ( Continuous( decl.Default.Type ) )
            {
                if ( changed )
                    StoreValue( view.Slot, decl, value );
                TrackContinuous( view.Target, view.Slot, before );
            }
            else if ( changed )
                Apply( view.Target, [&] { StoreValue( view.Slot, decl, value ); } );
            EndRow();
            if ( reset )
                Apply( view.Target, [&] { StoreValue( view.Slot, decl, decl.Default ); } );
            ImGui::PopID();
        }

        // The overrides the script no longer declares (or declares as another kind): not applied by the runtime
        // and reported once in the log; here they are shown with a button that drops them.
        void StaleRows( SlotView& view )
        {
            std::optional<std::string> drop;
            for ( const ScriptProperty& held : view.Slot.Properties )
            {
                const ScriptPropertyDecl* decl = Scripting::FindDecl( view.Schema, held.Name );
                if ( decl != nullptr && Fits( held, *decl ) )
                    continue;
                ImGui::PushID( held.Name.c_str() );
                BeginRow( held.Name.c_str(), {}, false, true );
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored( ThemeManager::GetWarningColor(), ICON_MDI_ALERT_CIRCLE_OUTLINE " %s",
                                    decl == nullptr
                                         ? "no longer declared by the script"
                                         : std::format( "now a {}, held a {}",
                                                        Scripting::PropertyTypeName( decl->Default.Type ),
                                                        Scripting::PropertyTypeName( held.Type ) )
                                                .c_str() );
                ImGui::SameLine( ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() - 2.0f );
                if ( IconButton( ICON_MDI_DELETE_OUTLINE, "Drop the value this entity holds" ) )
                    drop = held.Name;
                EndRow();
                ImGui::PopID();
            }
            if ( drop )
                Apply( view.Target,
                       [&] {
                           std::erase_if( view.Slot.Properties,
                                          [&]( const ScriptProperty& p ) { return p.Name == *drop; } );
                       } );
        }

        void PropertyRows( SlotView& view )
        {
            // Categories in the order they first appear; the uncategorised properties first, with no fold.
            std::vector<std::string> categories;
            for ( const ScriptPropertyDecl& decl : view.Schema.Properties )
                if ( std::find( categories.begin(), categories.end(), decl.Meta.Category ) == categories.end() )
                    categories.push_back( decl.Meta.Category );
            std::stable_partition( categories.begin(), categories.end(),
                                   []( const std::string& c ) { return c.empty(); } );

            for ( const std::string& category : categories )
            {
                ImGui::PushID( category.c_str() );
                const bool open = category.empty() || CategoryHeader( category );
                if ( open )
                {
                    for ( const ScriptPropertyDecl& decl : view.Schema.Properties )
                        if ( decl.Meta.Category == category && !decl.Meta.Advanced )
                            PropertyRow( view, decl );
                    const bool anyAdvanced =
                         std::any_of( view.Schema.Properties.begin(), view.Schema.Properties.end(),
                                      [&]( const ScriptPropertyDecl& d )
                                      { return d.Meta.Category == category && d.Meta.Advanced; } );
                    if ( anyAdvanced && ImGui::TreeNodeEx( "Advanced", ImGuiTreeNodeFlags_SpanAvailWidth ) )
                    {
                        for ( const ScriptPropertyDecl& decl : view.Schema.Properties )
                            if ( decl.Meta.Category == category && decl.Meta.Advanced )
                                PropertyRow( view, decl );
                        ImGui::TreePop();
                    }
                }
                ImGui::PopID();
            }
            StaleRows( view );
        }

        // ---- CallInEditor ------------------------------------------------------------------------------------

        // The last button error per slot (entity UUID + slot index), shown in the slot's error box until the
        // next press.
        std::unordered_map<std::string, std::string>& ButtonErrors()
        {
            static std::unordered_map<std::string, std::string> errors;
            return errors;
        }

        // Runs `function` on the entity in the EDIT world: a fresh script host bound to the world, the slot
        // loaded with the entity's values, the function called, the host dropped. One undo step.
        Common::BoolResultStr RunInEditWorld( ECS::Entity& entity, ::Desert::Core::Scene* scene,
                                              const ComponentEditContext& context, std::uint32_t slotIndex,
                                              const std::string& file, const std::vector<ScriptProperty>& values,
                                              const std::string& function )
        {
            if ( scene == nullptr )
                return Common::MakeError<bool>( std::string( "no world to run it in" ) );
            Scripting::ScriptEngine engine( scene, context.AssetMgr() );
            const auto              id     = static_cast<std::uint32_t>( entity.GetHandle() );
            Common::BoolResultStr   loaded = engine.LoadEntityScript( id, slotIndex, file );
            if ( !loaded )
                return loaded;
            engine.ApplyProperties( id, slotIndex, values );
            return engine.CallFunction( id, slotIndex, function );
        }

        void FunctionButtons( SlotView& view, ECS::Entity& entity, std::uint32_t slotIndex, bool playing,
                              const std::string& errorKey )
        {
            if ( view.Schema.EditorFunctions.empty() ||
                 !CategoryHeader( ICON_MDI_FUNCTION_VARIANT "  Functions" ) )
                return;
            ImGui::Indent( 8.0f );
            const float right = ImGui::GetContentRegionMax().x;
            for ( std::size_t i = 0; i < view.Schema.EditorFunctions.size(); ++i )
            {
                const std::string& function = view.Schema.EditorFunctions[i].Name;
                const std::string  text     = std::format( ICON_MDI_PLAY "  {}", function );
                const float        width =
                     ImGui::CalcTextSize( text.c_str() ).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                if ( i > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width < right )
                    ImGui::SameLine();
                if ( ImGui::Button( text.c_str() ) )
                {
                    if ( playing )
                        view.Slot.PendingEditorCalls.push_back( function );
                    else
                    {
                        const std::string                 file   = view.Slot.ResolvedPath().generic_string();
                        const std::vector<ScriptProperty> values = view.Slot.Properties;
                        Common::BoolResultStr             ran    = Common::MakeSuccess( true );
                        Apply( view.Target,
                               [&] {
                                   ran = RunInEditWorld( entity, view.Scene, view.Context, slotIndex, file, values,
                                                         function );
                               } );
                        if ( ran )
                        {
                            ButtonErrors().erase( errorKey );
                            ToastManager::Push( std::format( "Ran {}()", function ), ToastLevel::Info, 2.0f );
                        }
                        else
                            ButtonErrors()[errorKey] = ran.GetError();
                    }
                }
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s", std::format( "Call {}() {}", function,
                                                          playing ? "on the running script"
                                                                  : "on this entity in the edit world (undoable)" )
                                                  .c_str() );
            }
            ImGui::Unindent( 8.0f );
            ImGui::Spacing();
        }

        // ---- Header, status, error -------------------------------------------------------------------------

        struct Status
        {
            const char* Icon;
            const char* Text;
            ImVec4      Colour;
        };

        Status StatusOf( const ScriptSchema& schema, const ECS::ScriptSlot& slot, bool playing )
        {
            if ( schema.Status == ScriptStatus::NotFound )
                return { ICON_MDI_FILE_QUESTION_OUTLINE, "Not found", ThemeManager::GetWarningColor() };
            if ( schema.Status == ScriptStatus::Error || !slot.RuntimeError.empty() )
                return { ICON_MDI_ALERT_CIRCLE, "Error", ThemeManager::GetErrorColor() };
            if ( playing && slot.Started )
                return { ICON_MDI_PLAY, "Running", ThemeManager::GetSuccessColor() };
            return { ICON_MDI_CHECK_CIRCLE, "Ready", ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) };
        }

        // The message without the chunk prefix Luau puts before it (`<path>:<line>: `), which the box states as
        // a line number instead.
        std::string MessageOf( const std::string& error )
        {
            for ( std::size_t colon = error.find( ':' ); colon != std::string::npos;
                  colon             = error.find( ':', colon + 1 ) )
            {
                std::size_t end = colon + 1;
                while ( end < error.size() && error[end] >= '0' && error[end] <= '9' )
                    ++end;
                if ( end > colon + 1 && end < error.size() && error[end] == ':' )
                {
                    std::size_t text = end + 1;
                    while ( text < error.size() && error[text] == ' ' )
                        ++text;
                    return error.substr( text );
                }
            }
            return error;
        }

        // A framed box (error tint) around what it says: the title with the line, the message, the button that
        // opens the code editor at that line. Drawn behind its content through a channel split, so it is exactly
        // as tall as the message.
        void ErrorBox( const std::string& error, const char* title, const fs::path& file )
        {
            const int    line    = Scripting::ScriptErrorLine( error );
            const ImVec4 colour  = ThemeManager::GetErrorColor();
            const float  padding = 6.0f;
            ImDrawList*  dl      = ImGui::GetWindowDrawList();
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );
            ImGui::Dummy( ImVec2( 0.0f, padding * 0.5f ) );
            ImGui::Indent( padding * 2.0f );
            ImGui::BeginGroup();
            const std::string heading = line > 0
                                             ? std::format( ICON_MDI_ALERT_CIRCLE "  {} - line {}", title, line )
                                             : std::format( ICON_MDI_ALERT_CIRCLE "  {}", title );
            ImGui::TextColored( colour, "%s", heading.c_str() );
            ImGui::PushTextWrapPos( ImGui::GetContentRegionMax().x - padding * 2.0f );
            ImGui::TextUnformatted( MessageOf( error ).c_str() );
            ImGui::PopTextWrapPos();
            if ( !file.empty() )
            {
                const std::string open = line > 0 ? std::format( ICON_MDI_CODE_BRACES "  Open at line {}", line )
                                                  : std::string( ICON_MDI_CODE_BRACES "  Open in code editor" );
                if ( ImGui::Button( std::format( "{}##{}", open, title ).c_str() ) &&
                     !ContentBrowserUtils::ShellOpenInCodeEditor( file.generic_string(), line ) )
                    ToastManager::Push( "No VS Code: opened the script with the system's default application",
                                        ToastLevel::Info, 3.0f );
            }
            ImGui::EndGroup();
            ImGui::Unindent( padding * 2.0f );
            const ImVec2 min( ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x + padding -
                                   ImGui::GetScrollX(),
                              ImGui::GetItemRectMin().y - padding );
            const ImVec2 max( ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - padding,
                              ImGui::GetItemRectMax().y + padding );
            ImGui::Dummy( ImVec2( 0.0f, padding ) );
            dl->ChannelsSetCurrent( 0 );
            dl->AddRectFilled(
                 min, max,
                 ImGui::GetColorU32( ImVec4( colour.x * 0.30f, colour.y * 0.10f, colour.z * 0.10f, 0.55f ) ),
                 3.0f );
            dl->AddRect( min, max, ImGui::GetColorU32( ImVec4( colour.x, colour.y, colour.z, 0.55f ) ), 3.0f );
            dl->ChannelsMerge();
        }

        // The slot's bar: a SectionHeader with the script's name, the status chip painted into it, and the remove
        // button over its right end. True while it is open; `remove` set on the button.
        bool SlotHeader( const std::string& name, const Status& status, bool& remove )
        {
            const bool open = ImGuiUtilities::SectionHeader(
                 std::format( ICON_MDI_SCRIPT_TEXT_OUTLINE "  {}###slot", name ).c_str() );
            const ImVec2 barMin = ImGui::GetItemRectMin();
            const ImVec2 barMax = ImGui::GetItemRectMax();

            const float       button    = ImGui::GetFrameHeight();
            const std::string chip      = std::format( "{} {}", status.Icon, status.Text );
            const ImVec2      chipSize  = ImGui::CalcTextSize( chip.c_str() );
            const float       chipRight = barMax.x - button - 10.0f;
            const ImVec2      chipMin( chipRight - chipSize.x - 12.0f, barMin.y + 4.0f );
            const ImVec2      chipMax( chipRight, barMax.y - 4.0f );
            ImDrawList*       dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(
                 chipMin, chipMax,
                 ImGui::GetColorU32( ImVec4( status.Colour.x, status.Colour.y, status.Colour.z, 0.16f ) ), 3.0f );
            dl->AddText( ImVec2( chipMin.x + 6.0f, barMin.y + ( barMax.y - barMin.y - chipSize.y ) * 0.5f ),
                         ImGui::GetColorU32( status.Colour ), chip.c_str() );

            ImGui::SameLine( barMax.x - ImGui::GetWindowPos().x - button - 2.0f );
            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
            remove = ImGui::SmallButton( ICON_MDI_CLOSE "##remove" );
            ImGui::PopStyleColor();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Remove this script from the entity" );
            return open;
        }

        // The "Script" row: the picker over every project script (and a .lua dropped from the Content Browser),
        // then Open in code editor and Reload.
        void ScriptPickerRow( SlotView& view, bool playing )
        {
            BeginRow( "Script", "The .lua file this slot runs", false, true );
            const float buttons = ( ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x ) * 2.0f;
            ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - buttons );
            const std::string preview =
                 view.Slot.ScriptKey.empty() ? "Select script..." : ScriptLabel( view.Slot.ScriptKey );
            std::optional<std::string> chosen;
            if ( ImGui::BeginCombo( "##script", preview.c_str() ) )
            {
                const std::vector<std::string> scripts = ProjectScriptKeys();
                for ( const std::string& script : scripts )
                    if ( ImGui::Selectable( ScriptLabel( script ).c_str(), view.Slot.ScriptKey == script ) )
                        chosen = script;
                if ( scripts.empty() )
                    ImGui::TextDisabled( "no .lua under %s",
                                         Common::Constants::Path::SCRIPT_PATH.generic_string().c_str() );
                ImGui::EndCombo();
            }
            if ( ImGui::BeginDragDropTarget() )
            {
                if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( DragPayloads::AssetFile ) )
                {
                    const std::string dropped( static_cast<const char*>( payload->Data ) );
                    if ( fs::path( dropped ).extension() == ".lua" )
                        chosen = Common::AssetHandle::StableKeyForPath( fs::path( dropped ) );
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled( view.Slot.ScriptKey.empty() );
            if ( IconButton( ICON_MDI_CODE_BRACES, "Open in code editor" ) &&
                 !ContentBrowserUtils::ShellOpenInCodeEditor( view.Slot.ResolvedPath().generic_string(), 1 ) )
                ToastManager::Push( "No VS Code: opened the script with the system's default application",
                                    ToastLevel::Info, 3.0f );
            ImGui::SameLine();
            if ( IconButton( ICON_MDI_RELOAD, playing ? "Reload: the running script restarts from its top level"
                                                      : "Re-read the script's declarations" ) )
            {
                if ( playing )
                    view.Slot.Started = false;
                SchemaCache().erase( view.Slot.ResolvedPath().generic_string() );
            }
            ImGui::EndDisabled();
            EndRow();

            if ( chosen && *chosen != view.Slot.ScriptKey )
                Apply( view.Target,
                       [&]
                       {
                           view.Slot.ScriptKey = *chosen;
                           view.Slot.Started   = false;
                           view.Slot.Properties.clear(); // another script: its own declarations, no values yet
                           view.Slot.RuntimeError.clear();
                       } );
        }
    } // namespace

    void DrawScriptComponent( ECS::Entity& entity, ::Desert::Core::Scene* scene, const ComponentEditContext& context )
    {
        auto&            component = entity.GetComponent<ECS::ScriptComponent>();
        const bool       playing   = std::any_of( component.Scripts.begin(), component.Scripts.end(),
                                                  []( const ECS::ScriptSlot& s ) { return s.Started; } );
        const EditTarget target{ entity.GetComponent<ECS::UUIDComponent>().UUID, !playing };

        std::optional<std::size_t> removeIndex;
        for ( std::size_t i = 0; i < component.Scripts.size(); ++i )
        {
            ImGui::PushID( static_cast<int>( i ) );
            ECS::ScriptSlot&          slot = component.Scripts[i];
            static const ScriptSchema kNoScript{};
            const fs::path            file   = slot.ScriptKey.empty() ? fs::path() : slot.ResolvedPath();
            const ScriptSchema&       schema = slot.ScriptKey.empty() ? kNoScript : SchemaOf( file );
            const std::string name   = slot.ScriptKey.empty() ? std::string( "New Script" ) : file.stem().string();
            const Status      status = slot.ScriptKey.empty()
                                            ? Status{ ICON_MDI_FILE_QUESTION_OUTLINE, "No script",
                                                 ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) }
                                            : StatusOf( schema, slot, playing );
            bool              remove = false;
            if ( SlotHeader( name, status, remove ) )
            {
                ImGuiUtilities::ResetPropertyRows();
                SlotView view{ slot, schema, target, scene, context };
                ScriptPickerRow( view, playing );
                if ( !slot.ScriptKey.empty() )
                {
                    BeginRow( "Path", "The slot's reference: the script's root-tagged key", false, true );
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled( "%s", slot.ScriptKey.c_str() );
                    EndRow();

                    if ( !schema.Description.empty() )
                    {
                        ImGui::Spacing();
                        ImGui::Indent( 8.0f );
                        ImGui::PushTextWrapPos( ImGui::GetContentRegionMax().x - 8.0f );
                        ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
                        ImGui::TextUnformatted( schema.Description.c_str() );
                        ImGui::PopStyleColor();
                        ImGui::PopTextWrapPos();
                        ImGui::Unindent( 8.0f );
                        ImGui::Spacing();
                    }

                    const std::string errorKey =
                         std::format( "{}#{}", static_cast<std::uint64_t>( target.Entity ), i );
                    if ( schema.Status == ScriptStatus::Error )
                        ErrorBox( schema.Error, "Script error", file );
                    else if ( schema.Status == ScriptStatus::NotFound )
                        ErrorBox( schema.Error, "Script not found", {} );
                    if ( !slot.RuntimeError.empty() )
                        ErrorBox( slot.RuntimeError, "Runtime error", file );
                    if ( const auto pressed = ButtonErrors().find( errorKey ); pressed != ButtonErrors().end() )
                        ErrorBox( pressed->second, "Function error", file );

                    if ( schema.Status == ScriptStatus::Ok )
                    {
                        PropertyRows( view );
                        FunctionButtons( view, entity, static_cast<std::uint32_t>( i ), playing, errorKey );
                    }
                }
                ImGui::Spacing();
            }
            if ( remove )
                removeIndex = i;
            ImGui::PopID();
        }

        if ( removeIndex )
            Apply( target,
                   [&] {
                       component.Scripts.erase( component.Scripts.begin() +
                                                static_cast<std::ptrdiff_t>( *removeIndex ) );
                   } );

        ImGui::Spacing();
        if ( ImGuiUtilities::AccentButton( ICON_MDI_PLUS "  Add Script" ) )
            Apply( target, [&] { component.Scripts.emplace_back(); } );
    }
} // namespace Desert::Editor
