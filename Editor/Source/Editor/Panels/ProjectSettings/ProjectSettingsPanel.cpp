#include "ProjectSettingsPanel.hpp"

#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>

#include <Engine/Project/ProjectContext.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <utility>

#include <ImGui/imgui.h>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // A dropped Content Browser path as Game.json stores it: relative to the project directory, with '/'.
        // Refuses (with the reason) a file that is not a .webm — the only container the startup movie player
        // demuxes (Engine/Media/WebmDemuxer.hpp) — and a file outside the project, which a packaged game
        // would not carry.
        Common::ResultStr<std::string> MovieEntryFor( const std::filesystem::path& dropped,
                                                      const std::filesystem::path& projectDirectory )
        {
            std::string extension = dropped.extension().string();
            std::transform( extension.begin(), extension.end(), extension.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            if ( extension != ".webm" )
                return Common::MakeError<std::string>( std::format(
                     "{} is not a movie: startup movies are .webm (AV1 + Opus)", dropped.filename().string() ) );

            std::error_code             ec;
            const std::filesystem::path relative =
                 std::filesystem::relative( std::filesystem::weakly_canonical( dropped, ec ),
                                            std::filesystem::weakly_canonical( projectDirectory, ec ), ec );
            if ( ec || relative.empty() || *relative.begin() == ".." )
                return Common::MakeError<std::string>( std::format(
                     "{} is outside the project, so the packaged game would not carry it", dropped.string() ) );
            return Common::MakeSuccess( relative.generic_string() );
        }
    } // namespace

    // Starts CLOSED, like Scalability: it is opened on purpose from Window, not on every start.
    ProjectSettingsPanel::ProjectSettingsPanel() : IPanel( "Project Settings", /*showPanel=*/false )
    {
    }

    void ProjectSettingsPanel::OnUIRender()
    {
        if ( !::Desert::Project::ProjectContext::HasProject() )
        {
            ImGui::TextDisabled( "No project is open: project settings belong to a project's Config/Game.json." );
            return;
        }

        const std::string directory = ::Desert::Project::ProjectContext::Directory();
        if ( directory != m_LoadedFor )
        {
            m_LoadedFor = directory;
            m_Edit      = ::Desert::Project::CurrentGameSettings();
            m_Problem.clear();
        }

        ImGui::TextDisabled( "%s",
                             ( std::filesystem::path( directory ) / "Config" / "Game.json" ).string().c_str() );

        DrawGameSection();
        DrawMoviesSection();

        if ( !m_Problem.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s %s", ICON_MDI_ALERT, m_Problem.c_str() );
    }

    void ProjectSettingsPanel::DrawGameSection()
    {
        if ( !Utils::ImGuiUtilities::SectionHeader( "Game" ) )
            return;

        Utils::ImGuiUtilities::ResetPropertyRows();
        Utils::ImGuiUtilities::BeginPropertyRow( "Company", "The studio, as the credits and the menu show it." );
        char buffer[256] = {};
        std::strncpy( buffer, m_Edit.Company.c_str(), sizeof( buffer ) - 1 );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::InputText( "##Company", buffer, sizeof( buffer ) ) )
            m_Edit.Company = buffer;
        // Written when the field is left, not per keystroke: one edit, one write.
        if ( ImGui::IsItemDeactivatedAfterEdit() )
            Commit();
        Utils::ImGuiUtilities::EndPropertyRow();
    }

    void ProjectSettingsPanel::DrawMoviesSection()
    {
        if ( !Utils::ImGuiUtilities::SectionHeader( "Movies" ) )
            return;

        Utils::ImGuiUtilities::ResetPropertyRows();
        Utils::ImGuiUtilities::BeginPropertyRow( "Movies Are Skippable",
                                                 "A key or a click ends the current movie." );
        if ( ImGui::Checkbox( "##Skippable", &m_Edit.MoviesAreSkippable ) )
            Commit();
        Utils::ImGuiUtilities::EndPropertyRow();

        Utils::ImGuiUtilities::BeginPropertyRow( "Wait For Movies To Complete",
                                                 "Off: the movies end as soon as the game has loaded." );
        if ( ImGui::Checkbox( "##WaitForMovies", &m_Edit.WaitForMoviesToComplete ) )
            Commit();
        Utils::ImGuiUtilities::EndPropertyRow();

        ImGui::Spacing();
        ImGui::TextUnformatted( "Startup Movies" );
        Utils::ImGuiUtilities::Tooltip( "Played full screen at launch, in this order, before the first level." );

        // One row per movie: its order, the path Game.json stores, a mark when the file is not there, and
        // the three edits UE's array rows offer (up, down, remove). An edit is applied after the loop, so
        // the vector is never changed while it is being drawn.
        enum class Edit
        {
            None,
            Up,
            Down,
            Remove
        };
        Edit        edit   = Edit::None;
        std::size_t target = 0;
        const auto& movies = m_Edit.StartupMovies;
        const auto  root   = std::filesystem::path( m_LoadedFor );
        const float button = ImGui::GetFrameHeight();
        for ( std::size_t i = 0; i < movies.size(); ++i )
        {
            ImGui::PushID( static_cast<int>( i ) );
            ImGui::Text( "%zu", i );
            ImGui::SameLine();
            std::error_code ec;
            if ( std::filesystem::exists( root / movies[i], ec ) )
                ImGui::Text( "%s %s", ICON_MDI_MOVIE, movies[i].c_str() );
            else
            {
                ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s %s", ICON_MDI_ALERT,
                                    movies[i].c_str() );
                Utils::ImGuiUtilities::Tooltip( "No such file in the project: the game would refuse this movie." );
            }

            ImGui::SameLine( ImGui::GetContentRegionMax().x -
                             3.0f * ( button + ImGui::GetStyle().ItemSpacing.x ) );
            ImGui::BeginDisabled( i == 0 );
            if ( ImGui::Button( ICON_MDI_ARROW_UP, ImVec2( button, button ) ) )
                edit = Edit::Up, target = i;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled( i + 1 == movies.size() );
            if ( ImGui::Button( ICON_MDI_ARROW_DOWN, ImVec2( button, button ) ) )
                edit = Edit::Down, target = i;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ImGui::Button( ICON_MDI_CLOSE, ImVec2( button, button ) ) )
                edit = Edit::Remove, target = i;
            Utils::ImGuiUtilities::Tooltip( "Remove this movie from the list (the file stays)." );
            ImGui::PopID();
        }
        if ( movies.empty() )
            ImGui::TextDisabled( "No startup movies: the game shows its first level at once." );

        // The drop zone — UE adds an array element from the Content Browser by dragging the asset onto it.
        ImGui::Button( ICON_MDI_MOVIE "  Drop a .webm from the Content Browser to add it",
                       ImVec2( -1.0f, 2.0f * button ) );
        if ( ImGui::BeginDragDropTarget() )
        {
            if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( DragPayloads::AssetFile ) )
            {
                const std::string dropped( static_cast<const char*>( payload->Data ) );
                const auto        entry = MovieEntryFor( dropped, root );
                if ( !entry )
                    m_Problem = entry.GetError();
                else if ( std::find( movies.begin(), movies.end(), entry.GetValue() ) != movies.end() )
                    m_Problem = std::format( "{} is already in the list", entry.GetValue() );
                else
                {
                    m_Edit.StartupMovies.push_back( entry.GetValue() );
                    Commit();
                }
            }
            ImGui::EndDragDropTarget();
        }

        auto& list = m_Edit.StartupMovies;
        switch ( edit )
        {
            case Edit::None:
                return;
            case Edit::Up:
                std::swap( list[target], list[target - 1] );
                break;
            case Edit::Down:
                std::swap( list[target], list[target + 1] );
                break;
            case Edit::Remove:
                list.erase( list.begin() + static_cast<std::ptrdiff_t>( target ) );
                break;
        }
        Commit();
    }

    void ProjectSettingsPanel::Commit()
    {
        const auto saved = ::Desert::Project::SaveGameSettings( m_Edit );
        if ( saved )
            m_Problem.clear();
        else
            m_Problem = saved.GetError();
    }
} // namespace Desert::Editor
