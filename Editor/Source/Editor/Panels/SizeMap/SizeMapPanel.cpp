#include "SizeMapPanel.hpp"

#include <Editor/Core/FileType.hpp>
#include <Editor/Core/PanelRequests.hpp>
#include <Editor/Panels/FileExplorer/FileTypeInfo.hpp>

#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/AssetHandle.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

namespace Desert::Editor
{
    namespace
    {
        // THE ONE PANEL THE CONTENT BROWSER OPENS (UE: FGlobalTabmanager::TryInvokeTab on the size map tab). The
        // registry owns the panel; this only names it while it lives.
        SizeMapPanel*& SizeMapPanelInstance()
        {
            static SizeMapPanel* panel = nullptr;
            return panel;
        }

        // The stable keys the request maps: each asset's own, and every row under each folder. A folder's rows are
        // those whose key starts with the key a file directly inside it would get, less that file's name -
        // "assets:VFX/" for a folder, "assets:" for a content root itself (StableKeyForPath has no relative path
        // to give a root, so the root's own key is untagged). A redirector row is a move's trace, not content the
        // folder holds (Common::Content::ReferrersOf skips it for the same reason).
        Common::ResultStr<std::vector<std::string>>
        SizeMapRootKeys( const Common::Utils::AssetRegistry&       registry,
                         const std::vector<std::filesystem::path>& assets,
                         const std::vector<std::filesystem::path>& folders )
        {
            using Keys = std::vector<std::string>;
            Keys keys;
            for ( const std::filesystem::path& asset : assets )
            {
                std::string key = Common::AssetHandle::StableKeyForPath( asset );
                if ( !Common::AssetHandle::IsProjectRelativeKey( key ) )
                    return Common::MakeError<Keys>(
                         std::format( "Size Map: '{}' is outside every content root", asset.generic_string() ) );
                keys.push_back( std::move( key ) );
            }
            const std::string_view redirector =
                 Common::Content::KindName( Common::Content::ContentKind::Redirector );
            for ( const std::filesystem::path& folder : folders )
            {
                const std::string probe = Common::AssetHandle::StableKeyForPath( folder / "_" );
                if ( !Common::AssetHandle::IsProjectRelativeKey( probe ) )
                    return Common::MakeError<Keys>( std::format(
                         "Size Map: the folder '{}' is outside every content root", folder.generic_string() ) );
                const std::string prefix = probe.substr( 0, probe.size() - 1 );
                std::size_t       found  = 0;
                for ( const Common::Utils::AssetRegistryEntry& row : registry.Entries() )
                {
                    if ( row.Key.starts_with( prefix ) && row.Kind != redirector )
                    {
                        keys.push_back( row.Key );
                        ++found;
                    }
                }
                if ( found == 0 )
                    return Common::MakeError<Keys>( std::format(
                         "Size Map: the folder '{}' holds no registry content", folder.generic_string() ) );
            }
            return Common::MakeSuccess( std::move( keys ) );
        }

        // UE colours a box by its asset type (the type actions' colour); ours is the Content Browser's own type
        // colour (FileTypeInfoOf), so a box and its tile agree. The asset's own bytes are a darker shade of it.
        ImU32 SizeMapFill( const SizeMap::Node& node )
        {
            if ( node.Role == SizeMap::NodeRole::Shared || node.Role == SizeMap::NodeRole::Group )
                return IM_COL32( 70, 70, 76, 255 );
            const std::size_t colon = node.Key.find( ':' );
            std::string       extension =
                 std::filesystem::path( colon == std::string::npos ? node.Key : node.Key.substr( colon + 1 ) )
                      .extension()
                      .string();
            if ( !extension.empty() )
                extension.erase( 0, 1 );
            std::transform( extension.begin(), extension.end(), extension.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            std::optional<Common::Content::ContentKind> kind;
            if ( node.Kind == Common::Content::KindName( Common::Content::ContentKind::Skybox ) )
                kind = Common::Content::ContentKind::Skybox;
            const ImVec4 type  = FileTypeInfoOf( FileTypeOfContent( extension, kind ) ).Colour;
            const float  shade = node.Role == SizeMap::NodeRole::Self ? 0.45f : 0.65f;
            return ImGui::ColorConvertFloat4ToU32(
                 ImVec4( type.x * shade, type.y * shade, type.z * shade, 1.0f ) );
        }

        std::string SizeMapLabel( const SizeMap::Node& node )
        {
            if ( node.Role == SizeMap::NodeRole::Group || node.Assets == 0 )
                return std::format( "{}  ({})", node.Name, SizeMap::FormatBytes( node.TotalBytes ) );
            return std::format( "{}  ({}, {} dependencies)", node.Name, SizeMap::FormatBytes( node.TotalBytes ),
                                node.Assets );
        }

        constexpr float kSizeMapHeader   = 18.0f; // the title strip of a box that shows its children
        constexpr float kSizeMapMinOpenW = 64.0f; // a box smaller than this is drawn closed
        constexpr float kSizeMapMinOpenH = 42.0f;
        constexpr int   kSizeMapMaxDepth = 8;
    } // namespace

    SizeMapPanel::SizeMapPanel() : IPanel( "Size Map", /*showPanel=*/false )
    {
        SizeMapPanelInstance() = this;
    }

    SizeMapPanel::~SizeMapPanel()
    {
        if ( SizeMapPanelInstance() == this )
            SizeMapPanelInstance() = nullptr;
    }

    Common::BoolResultStr SizeMapPanel::ShowFor( std::vector<std::filesystem::path> assets,
                                                 std::vector<std::filesystem::path> folders )
    {
        m_Assets  = std::move( assets );
        m_Folders = std::move( folders );
        // Shown and brought forward even when refused: the window says why (UE: TryInvokeTab, then the map).
        Core::PanelRequests::Open( GetName() );
        return Rebuild();
    }

    Common::BoolResultStr SizeMapPanel::Rebuild()
    {
        m_ViewHistory.clear();
        m_ViewRoot = -1;
        m_Tree     = {};
        m_Error.clear();
        // A copy taken under the registry's lock: the async loader's completions note rows from workers.
        m_Registry = Assets::ContentRegistry::Snapshot();
        auto keys  = SizeMapRootKeys( m_Registry, m_Assets, m_Folders );
        if ( !keys )
        {
            m_Error = keys.GetError();
            return Common::MakeError<bool>( m_Error );
        }
        auto built = SizeMap::Build( m_Registry, keys.ExtractValue() );
        if ( !built )
        {
            m_Error = built.GetError();
            return Common::MakeError<bool>( m_Error );
        }
        m_Tree     = built.ExtractValue();
        m_ViewRoot = m_Tree.Root;
        return Common::MakeSuccess( true );
    }

    void SizeMapPanel::OnUIRender()
    {
        if ( ImGui::Button( "Refresh" ) && ( !m_Assets.empty() || !m_Folders.empty() ) )
            static_cast<void>( Rebuild() ); // a refusal is kept in m_Error and drawn below
        ImGui::SameLine();
        const bool canGoUp = !m_ViewHistory.empty();
        ImGui::BeginDisabled( !canGoUp );
        if ( ImGui::Button( "Up" ) && canGoUp )
        {
            m_ViewRoot = m_ViewHistory.back();
            m_ViewHistory.pop_back();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled( "Disk size (asset registry). Double-click a box to look inside it." );

        if ( !m_Error.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", m_Error.c_str() );
        if ( m_Tree.Empty() || m_ViewRoot < 0 )
        {
            ImGui::TextDisabled( "Content Browser: right-click an asset or a folder > Size Map..." );
            return;
        }

        // Where the view stands: the chain from the map's root to the box looked into.
        std::string crumbs;
        for ( int at = m_ViewRoot; at >= 0; at = m_Tree.Nodes[static_cast<std::size_t>( at )].Parent )
            crumbs = crumbs.empty() ? SizeMapLabel( m_Tree.Nodes[static_cast<std::size_t>( at )] )
                                    : std::format( "{}  >  {}", m_Tree.Nodes[static_cast<std::size_t>( at )].Name,
                                                   crumbs );
        ImGui::TextUnformatted( crumbs.c_str() );

        if ( !m_Tree.Unresolved.empty() &&
             ImGui::CollapsingHeader( std::format( "{} reference(s) the registry could not follow###sizemapUnresolved",
                                                   m_Tree.Unresolved.size() )
                                           .c_str() ) )
        {
            for ( const std::string& line : m_Tree.Unresolved )
                ImGui::BulletText( "%s", line.c_str() );
        }

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 avail  = ImGui::GetContentRegionAvail();
        if ( avail.x < 32.0f || avail.y < 32.0f )
            return;
        ImGui::InvisibleButton( "##sizemapCanvas", avail );
        const bool canvasHovered = ImGui::IsItemHovered();

        m_Hovered            = -1;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect( origin, ImVec2( origin.x + avail.x, origin.y + avail.y ), true );
        DrawBox( m_ViewRoot, SizeMap::Rect{ origin.x, origin.y, avail.x, avail.y }, 0 );
        drawList->PopClipRect();

        if ( !canvasHovered || m_Hovered < 0 )
            return;
        DrawTooltip( m_Hovered );
        if ( !ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
            return;
        // Into the box under the cursor; a box with nothing inside (a leaf, an asset's own bytes) opens its parent.
        int target = m_Hovered;
        if ( m_Tree.Nodes[static_cast<std::size_t>( target )].Children.empty() )
            target = m_Tree.Nodes[static_cast<std::size_t>( target )].Parent;
        if ( target >= 0 && target != m_ViewRoot )
        {
            m_ViewHistory.push_back( m_ViewRoot );
            m_ViewRoot = target;
        }
    }

    void SizeMapPanel::DrawBox( const int node, const SizeMap::Rect& box, const int depth )
    {
        const SizeMap::Node& data     = m_Tree.Nodes[static_cast<std::size_t>( node )];
        ImDrawList*          drawList = ImGui::GetWindowDrawList();
        const ImVec2         topLeft( box.X, box.Y );
        const ImVec2         bottomRight( box.X + box.W, box.Y + box.H );
        const bool           isView = node == m_ViewRoot;

        if ( !isView )
        {
            drawList->AddRectFilled( topLeft, bottomRight, SizeMapFill( data ) );
            drawList->AddRect( topLeft, bottomRight, IM_COL32( 20, 20, 22, 255 ) );
            if ( ImGui::IsMouseHoveringRect( topLeft, bottomRight ) )
                m_Hovered = node; // children are drawn after their parent, so the deepest box wins
        }

        const bool open = isView || ( !data.Children.empty() && depth < kSizeMapMaxDepth &&
                                      box.W >= kSizeMapMinOpenW && box.H >= kSizeMapMinOpenH );
        if ( !isView && box.W > 28.0f && box.H > 14.0f )
        {
            const std::string label = SizeMapLabel( data );
            drawList->PushClipRect( topLeft, bottomRight, true );
            drawList->AddText( ImVec2( box.X + 4.0f, box.Y + 2.0f ), IM_COL32( 235, 235, 235, 255 ), label.c_str() );
            drawList->PopClipRect();
        }
        if ( !open )
            return;

        const float         header = isView ? 0.0f : kSizeMapHeader;
        const SizeMap::Rect inner{ box.X + 2.0f, box.Y + header, std::max( 0.0f, box.W - 4.0f ),
                                   std::max( 0.0f, box.H - header - 2.0f ) };
        for ( const SizeMap::Placed& child : SizeMap::Squarify( m_Tree, node, inner ) )
            DrawBox( child.Node, child.Box, depth + 1 );
    }

    void SizeMapPanel::DrawTooltip( const int node ) const
    {
        const SizeMap::Node& data = m_Tree.Nodes[static_cast<std::size_t>( node )];
        ImGui::BeginTooltip();
        ImGui::TextUnformatted( data.Name.c_str() );
        switch ( data.Role )
        {
            case SizeMap::NodeRole::Shared:
                ImGui::TextDisabled( "Assets reached from more than one of the mapped assets" );
                break;
            case SizeMap::NodeRole::Group:
                break;
            case SizeMap::NodeRole::Self:
                ImGui::TextDisabled( "The asset's own file, without what it names" );
                [[fallthrough]];
            case SizeMap::NodeRole::Asset:
                ImGui::TextUnformatted( data.Key.c_str() );
                ImGui::Text( "Kind: %s", data.Kind.c_str() );
                ImGui::Text( "Own size: %s", SizeMap::FormatBytes( data.OwnBytes ).c_str() );
                break;
        }
        ImGui::Text( "Total size: %s", SizeMap::FormatBytes( data.TotalBytes ).c_str() );
        if ( data.Assets > 0 )
            ImGui::Text( "Dependencies: %zu", data.Assets );
        ImGui::EndTooltip();
    }

    Common::BoolResultStr OpenSizeMap( std::vector<std::filesystem::path> assets,
                                       std::vector<std::filesystem::path> folders )
    {
        SizeMapPanel* panel = SizeMapPanelInstance();
        if ( panel == nullptr )
            return Common::MakeError<bool>( "Size Map: this editor has no Size Map panel" );
        return panel->ShowFor( std::move( assets ), std::move( folders ) );
    }
} // namespace Desert::Editor
