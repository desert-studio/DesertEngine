#include "ContentChunksPanel.hpp"

#include <ImGui/imgui.h>

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Packaging/ProjectChunkScheme.hpp>

#include <Engine/Assets/ContentRegistry.hpp>

#include <string>

namespace Desert::Editor
{
    namespace
    {
        const ImVec4 kRefused( 1.0f, 0.4f, 0.4f, 1.0f );
        const ImVec4 kGood( 0.5f, 0.9f, 0.5f, 1.0f );

        // The session's last action — or its refusal, which names what was refused. The Build Settings
        // panel draws the same string from the same session.
        void DrawLastAction( const Common::Content::ChunkSchemeSession& session )
        {
            if ( !session.LastAction().empty() )
                ImGui::TextWrapped( "Last action: %s", session.LastAction().c_str() );
        }
    } // namespace

    void ContentChunksPanel::RefreshPreview()
    {
        auto& session     = ProjectChunkScheme();
        m_PreviewRevision = session.Revision();
        m_PreviewNames.clear();
        m_PreviewFolders.clear();
        m_PreviewUnresolved = 0;
        m_PreviewError.clear();
        if ( session.GetStatus() != Common::Content::ChunkSchemeSession::Status::Loaded )
            return;

        // THE PACKAGER'S DERIVATION, on the packager's registry (GamePackager's PlanTheDivision reads
        // the same ContentRegistry::Get()), over the draft — so an unsaved edit previews what Build will
        // do once it is saved, and a draft Build would refuse shows Build's refusal.
        const auto& registry = Assets::ContentRegistry::Get();
        const auto  plan     = Common::Content::BuildChunkPlan( registry, session.Draft() );
        if ( !plan )
        {
            m_PreviewError = plan.GetError();
            return;
        }
        m_PreviewNames      = plan.GetValue().Names();
        m_PreviewFolders    = Common::Content::SummarizeChunkFolders( registry, plan.GetValue() );
        m_PreviewUnresolved = plan.GetValue().UnresolvedEdges().size();
    }

    void ContentChunksPanel::OnUIRender()
    {
        using Status  = Common::Content::ChunkSchemeSession::Status;
        auto& session = ProjectChunkScheme();

        ImGui::TextUnformatted( session.Path().string().c_str() );
        ImGui::PushTextWrapPos( 0.0f );
        ImGui::TextColored( session.GetStatus() == Status::Loaded ? kGood : kRefused, "%s",
                            session.LoadMessage().c_str() );
        ImGui::PopTextWrapPos();

        if ( ImGui::SmallButton( ICON_MDI_REFRESH "  Reload" ) )
        {
            session.Reload();
            RefreshPreview();
        }
        if ( session.GetStatus() == Status::Missing )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( ICON_MDI_FILE_PLUS "  Create default ContentChunks.json" ) )
                (void)session.CreateDefault();
        }
        if ( session.GetStatus() != Status::Loaded )
        {
            DrawLastAction( session );
            return;
        }

        ImGui::SameLine();
        ImGui::BeginDisabled( !session.Dirty() );
        if ( ImGui::SmallButton( ICON_MDI_CONTENT_SAVE "  Save" ) )
            (void)session.Save( Assets::ContentRegistry::Get() );
        ImGui::SameLine();
        if ( ImGui::SmallButton( ICON_MDI_UNDO "  Revert" ) )
            session.Revert();
        ImGui::EndDisabled();
        if ( session.Dirty() )
        {
            ImGui::SameLine();
            ImGui::TextColored( kRefused, "unsaved — Build reads the file, not this draft" );
        }

        // ── Edit fields: their text is handed to the session and nowhere else ─────────────────────
        ImGui::Separator();
        ImGui::SetNextItemWidth( 220.0f );
        ImGui::InputText( "Chunk name", m_ChunkNameInput.data(), m_ChunkNameInput.size() );
        ImGui::SameLine();
        if ( ImGui::SmallButton( ICON_MDI_PLUS "  Add chunk" ) )
            (void)session.AddChunk( m_ChunkNameInput.data() );
        ImGui::SetNextItemWidth( 360.0f );
        ImGui::InputText( "Stable key", m_KeyInput.data(), m_KeyInput.size() );
        ImGui::SameLine();
        if ( ImGui::SmallButton( ICON_MDI_PIN "  Pin to Base" ) )
            (void)session.AddPin( m_KeyInput.data() );
        ImGui::TextDisabled( "Stable keys look like assets:Materials/M_Rock.demat (the registry's own keys)." );

        // ── The chunks: Base first, always, then every declared chunk with its roots ──────────────
        ImGui::Separator();
        const Common::Content::ChunkScheme& draft = session.Draft();
        ImGui::BulletText( "#0  %s — every key no chunk claims, every key two chunks reach, every pin",
                           std::string( Common::Content::BASE_CHUNK_NAME ).c_str() );
        for ( std::size_t pin = 0; pin < draft.AlwaysBase.size(); ++pin )
        {
            ImGui::PushID( static_cast<int>( pin ) );
            ImGui::Text( "      pinned: %s", draft.AlwaysBase[pin].c_str() );
            ImGui::SameLine();
            if ( ImGui::SmallButton( ICON_MDI_CLOSE "##unpin" ) )
                (void)session.RemovePin( pin );
            ImGui::PopID();
        }
        for ( std::size_t chunk = 0; chunk < draft.Chunks.size(); ++chunk )
        {
            const Common::Content::ChunkRule& rule = draft.Chunks[chunk];
            ImGui::PushID( static_cast<int>( 1000 + chunk ) );
            ImGui::BulletText( "#%zu  %s", chunk + 1, rule.Name.c_str() );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Add root from key" ) )
                (void)session.AddRoot( chunk, m_KeyInput.data() );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Rename to chunk name" ) )
                (void)session.RenameChunk( chunk, m_ChunkNameInput.data() );
            ImGui::SameLine();
            if ( ImGui::SmallButton( ICON_MDI_DELETE "##remove" ) )
                (void)session.RemoveChunk( chunk );
            if ( rule.Roots.empty() )
                ImGui::TextColored( kRefused, "      no roots yet — Save refuses a chunk that would ship empty" );
            for ( std::size_t root = 0; root < rule.Roots.size(); ++root )
            {
                ImGui::PushID( static_cast<int>( root ) );
                ImGui::Text( "      root: %s", rule.Roots[root].c_str() );
                ImGui::SameLine();
                if ( ImGui::SmallButton( ICON_MDI_CLOSE "##root" ) )
                    (void)session.RemoveRoot( chunk, root );
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        DrawLastAction( session );

        // ── Where every content folder lands, derived exactly as Build derives it ─────────────────
        ImGui::Separator();
        if ( m_PreviewRevision != session.Revision() )
            RefreshPreview();
        ImGui::TextUnformatted( "Files per folder and archive (the draft, against the cooked registry)" );
        ImGui::SameLine();
        if ( ImGui::SmallButton( ICON_MDI_REFRESH "##preview" ) )
            RefreshPreview();
        if ( !m_PreviewError.empty() )
        {
            ImGui::PushTextWrapPos( 0.0f );
            ImGui::TextColored( kRefused, "Build would refuse this scheme: %s", m_PreviewError.c_str() );
            ImGui::PopTextWrapPos();
            return;
        }
        if ( m_PreviewUnresolved > 0 )
            ImGui::TextColored( kRefused,
                                "%zu dependency handle(s) name no registry row (AssetReferences owns them)",
                                m_PreviewUnresolved );

        const int columns = static_cast<int>( m_PreviewNames.size() ) + 1;
        if ( ImGui::BeginTable( "##folders", columns,
                                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY ) )
        {
            ImGui::TableSetupScrollFreeze( 0, 1 );
            ImGui::TableSetupColumn( "Folder" );
            for ( const std::string& name : m_PreviewNames )
                ImGui::TableSetupColumn( name.c_str() );
            ImGui::TableHeadersRow();
            for ( const Common::Content::ChunkFolderRow& row : m_PreviewFolders )
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( row.Folder.c_str() );
                for ( const std::size_t count : row.FilesPerChunk )
                {
                    ImGui::TableNextColumn();
                    if ( count == 0 )
                        ImGui::TextDisabled( "-" );
                    else
                        ImGui::Text( "%zu", count );
                }
            }
            ImGui::EndTable();
        }
    }
} // namespace Desert::Editor
