#include "SizeMapPanel.hpp"

#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Core/AssetHandle.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <format>
#include <functional>
#include <mutex>
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

        // A copy taken under the registry's lock: the async loader's completions note rows from workers.
        Common::Utils::AssetRegistry SizeMapRegistrySnapshot()
        {
            Assets::ContentRegistry::Detail::State& state = Assets::ContentRegistry::Detail::Get_();
            const std::lock_guard<std::mutex>       lock( state.Mutex );
            return state.Registry;
        }

        ImU32 SizeMapFill( const SizeMap::Node& node )
        {
            if ( node.Role == SizeMap::NodeRole::Shared || node.Role == SizeMap::NodeRole::Group )
                return IM_COL32( 70, 70, 76, 255 );
            const float hue = static_cast<float>( std::hash<std::string>{}( node.Kind ) % 360u ) / 360.0f;
            const float value = node.Role == SizeMap::NodeRole::Self ? 0.42f : 0.55f;
            float       r = 0.0f, g = 0.0f, b = 0.0f;
            ImGui::ColorConvertHSVtoRGB( hue, 0.45f, value, r, g, b );
            return ImGui::ColorConvertFloat4ToU32( ImVec4( r, g, b, 1.0f ) );
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

    Common::BoolResultStr SizeMapPanel::ShowFor( const std::vector<std::filesystem::path>& files )
    {
        m_Registry = SizeMapRegistrySnapshot();
        std::vector<std::string> keys;
        for ( const std::filesystem::path& file : files )
        {
            const std::string key = Common::AssetHandle::StableKeyForPath( file );
            if ( key.empty() )
                return Common::MakeError<bool>( std::format( "Size Map: '{}' is outside every content root", file.generic_string() ) );
            std::error_code ec;
            if ( !std::filesystem::is_directory( file, ec ) )
            {
                keys.push_back( key );
                continue;
            }
            // A FOLDER STANDS FOR ITS ASSETS (UE: Size Map on a folder maps every asset under it).
            const std::string prefix = key + "/";
            std::size_t       found  = 0;
            for ( const Common::Utils::AssetRegistryEntry& row : m_Registry.Entries() )
            {
                if ( row.Key.starts_with( prefix ) )
                {
                    keys.push_back( row.Key );
                    ++found;
                }
            }
            if ( found == 0 )
                return Common::MakeError<bool>( std::format( "Size Map: the folder '{}' holds no registry content", file.generic_string() ) );
        }
        m_RootKeys = std::move( keys );
        GetVisibility() = true;
        return Rebuild();
    }

    Common::BoolResultStr SizeMapPanel::Rebuild()
    {
        m_ViewHistory.clear();
        m_ViewRoot = -1;
        m_Error.clear();
        auto built = SizeMap::Build( m_Registry, m_RootKeys );
        if ( !built )
        {
            m_Tree  = {};
            m_Error = built.GetError();
            return Common::MakeError<bool>( m_Error );
        }
        m_Tree     = std::move( built.GetValue() );
        m_ViewRoot = m_Tree.Root;
        return Common::MakeSuccess( true );
    }

    void SizeMapPanel::OnUIRender()
    {
        if ( ImGui::Button( "Refresh" ) && !m_RootKeys.empty() )
        {
            m_Registry = SizeMapRegistrySnapshot();
            static_cast<void>( Rebuild() ); // a refusal is kept in m_Error and drawn below
        }
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

        m_Hovered = -1;
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
        const ImVec2         min( box.X, box.Y );
        const ImVec2         max( box.X + box.W, box.Y + box.H );
        const bool           isView = node == m_ViewRoot;

        if ( !isView )
        {
            drawList->AddRectFilled( min, max, SizeMapFill( data ) );
            drawList->AddRect( min, max, IM_COL32( 20, 20, 22, 255 ) );
            if ( ImGui::IsMouseHoveringRect( min, max ) )
                m_Hovered = node; // children are drawn after their parent, so the deepest box wins
        }

        const bool open = isView || ( !data.Children.empty() && depth < kSizeMapMaxDepth &&
                                      box.W >= kSizeMapMinOpenW && box.H >= kSizeMapMinOpenH );
        const std::string label = SizeMapLabel( data );
        if ( !isView && box.W > 28.0f && box.H > 14.0f )
        {
            drawList->PushClipRect( min, max, true );
            drawList->AddText( ImVec2( box.X + 4.0f, box.Y + 2.0f ), IM_COL32( 235, 235, 235, 255 ), label.c_str() );
            drawList->PopClipRect();
        }
        if ( !open )
            return;

        const float          header = isView ? 0.0f : kSizeMapHeader;
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

    Common::BoolResultStr OpenSizeMap( const std::vector<std::filesystem::path>& files )
    {
        SizeMapPanel* panel = SizeMapPanelInstance();
        if ( panel == nullptr )
            return Common::MakeError<bool>( "Size Map: this editor has no Size Map panel" );
        return panel->ShowFor( files );
    }
} // namespace Desert::Editor
