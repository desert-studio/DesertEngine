#include "ProjectSettingsPanel.hpp"

#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>

#include <Projects/EngineConfig.hpp>
#include <Projects/ProjectContext.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <string_view>
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
            m_Edit          = ::Desert::Project::CurrentGameSettings();
            m_EditRendering      = ::Desert::Project::ProjectRendererSettings();
            m_EditWorldPartition = ::Desert::Project::ProjectWorldPartitionSettings();
            m_Problem.clear();
        }

        ImGui::TextDisabled( "%s",
                             ( std::filesystem::path( directory ) / "Config" / "Game.json" ).string().c_str() );

        DrawGameSection();
        DrawMoviesSection();
        DrawRenderingSection();
        DrawWorldPartitionSection();

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

    void ProjectSettingsPanel::DrawRenderingSection()
    {
        if ( !Utils::ImGuiUtilities::SectionHeader( "Rendering" ) )
            return;

        ImGui::TextDisabled( "%s", ( std::filesystem::path( m_LoadedFor ) / "Config" / "Engine.json" ).string().c_str() );
        Utils::ImGuiUtilities::ResetPropertyRows();
        Utils::ImGuiUtilities::BeginPropertyRow(
             "TSR Thin Geometry Detection",
             "UE r.TSR.ThinGeometryDetection: TSR finds sub-pixel geometry (foliage, wires) in the base passes' "
             "coverage and keeps more of its history there. Costs one pass and one history." );
        if ( ImGui::Checkbox( "##TsrThinGeometryDetection", &m_EditRendering.TsrThinGeometryDetection ) )
            CommitRendering();
        Utils::ImGuiUtilities::EndPropertyRow();
        Utils::ImGuiUtilities::BeginPropertyRow(
             "Game Viewport High DPI",
             "UE r.SecondaryScreenPercentage.GameViewport: off -- the packaged game renders at the window's logical "
             "resolution (display pixels / DPI scale) and upscales at present; on -- at the display's native pixels "
             "(a Retina window costs four times the targets and histories)." );
        if ( ImGui::Checkbox( "##GameViewportHighDPI", &m_EditRendering.GameViewportHighDPI ) )
            CommitRendering();
        Utils::ImGuiUtilities::EndPropertyRow();

        // UE Project Settings > Engine > Texture Encoding. Each effort is part of the texture DDC key, so a change
        // re-derives the textures built at it (the editor at the next miss, the cook at the next TextureCook).
        auto& encoding = m_EditRendering.TextureEncoding;
        Utils::ImGuiUtilities::BeginPropertyRow(
             "Texture Fast Encode Effort",
             "BC7 search effort (0..1) of a fast encode: what the editor builds on a DDC miss while you wait." );
        ImGui::SliderFloat( "##TextureFastEffort", &encoding.FastEffort, 0.0f, 1.0f, "%.2f" );
        if ( ImGui::IsItemDeactivatedAfterEdit() )
            CommitRendering();
        Utils::ImGuiUtilities::EndPropertyRow();
        Utils::ImGuiUtilities::BeginPropertyRow(
             "Texture Final Encode Effort",
             "BC7 search effort (0..1) of a final encode: what TextureCook and the packager build, what ships." );
        ImGui::SliderFloat( "##TextureFinalEffort", &encoding.FinalEffort, 0.0f, 1.0f, "%.2f" );
        if ( ImGui::IsItemDeactivatedAfterEdit() )
            CommitRendering();
        Utils::ImGuiUtilities::EndPropertyRow();
        const char* speeds[] = { "Final", "Final If Available", "Fast" };
        const auto  speedRow = [&]( const char* label, const char* tip, const char* id,
                                   ::Desert::Project::TextureEncodeSpeed& speed )
        {
            Utils::ImGuiUtilities::BeginPropertyRow( label, tip );
            int at = static_cast<int>( speed );
            if ( ImGui::Combo( id, &at, speeds, IM_ARRAYSIZE( speeds ) ) )
            {
                speed = static_cast<::Desert::Project::TextureEncodeSpeed>( at );
                CommitRendering();
            }
            Utils::ImGuiUtilities::EndPropertyRow();
        };
        speedRow( "Texture Editor Uses",
                  "Final If Available: a final entry a cook stored is used as it is; a miss is built fast.",
                  "##TextureEditorUses", encoding.EditorUses );
        speedRow( "Texture Cook Uses", "What TextureCook and the packager build.", "##TextureCookUses",
                  encoding.CookUses );
    }

    void ProjectSettingsPanel::CommitRendering()
    {
        const auto saved = ::Desert::Project::SaveRendererSettings( m_EditRendering );
        if ( saved )
            m_Problem.clear();
        else
            m_Problem = saved.GetError();
    }

    // UE Project Settings > Engine > World Partition (the wp.Runtime.* values a project sets in
    // DefaultEngine.ini): the WorldPartition section of Config/Engine.json, read by the WorldStreamer when Play or
    // the game begins. A value this platform's layer (Config/<Platform>/Engine.json) also sets is marked: this
    // host runs that one.
    void ProjectSettingsPanel::DrawWorldPartitionSection()
    {
        if ( !Utils::ImGuiUtilities::SectionHeader( "World Partition" ) )
            return;

        const ::Desert::Project::EngineConfigLayers& layers = ::Desert::Project::CurrentEngineConfig();
        auto&                                        edit   = m_EditWorldPartition;
        const auto                                   endRow = [&]( std::string_view key )
        {
            const std::string path = std::format( "WorldPartition.{}", key );
            for ( const std::string& set : layers.PlatformKeys )
                if ( set == path || set == "WorldPartition" )
                {
                    ImGui::TextDisabled( "%s", std::format( "{} sets it on {}", layers.PlatformFile,
                                                            ::Desert::Project::PlatformConfigName() )
                                                    .c_str() );
                    break;
                }
            Utils::ImGuiUtilities::EndPropertyRow();
        };
        const auto commitOnEdit = [&]
        {
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                CommitWorldPartition();
        };

        Utils::ImGuiUtilities::ResetPropertyRows();
        Utils::ImGuiUtilities::BeginPropertyRow(
             "Block On Slow Streaming",
             "UE wp.Runtime.BlockOnSlowStreaming: while the cell under a streaming source is not loaded, the game "
             "stops behind the loading overlay. Off: play goes on over the HLOD and the log says so." );
        if ( ImGui::Checkbox( "##BlockOnSlowStreaming", &edit.BlockOnSlowStreaming ) )
            CommitWorldPartition();
        endRow( "BlockOnSlowStreaming" );

        Utils::ImGuiUtilities::BeginPropertyRow(
             "Slow Streaming Ratio",
             "UE wp.Runtime.BlockOnSlowStreamingRatio: a cell's distance over a source's loading range. Times the "
             "warning factor, it is where streaming counts as slow while a wanted cell is not loaded yet." );
        ImGui::DragFloat( "##SlowStreamingRatio", &edit.SlowStreamingRatio, 0.01f, 0.0f, 1.0f, "%.2f" );
        commitOnEdit();
        endRow( "SlowStreamingRatio" );

        Utils::ImGuiUtilities::BeginPropertyRow( "Slow Streaming Warning Factor",
                                                 "UE wp.Runtime.BlockOnSlowStreamingWarningFactor (>= 1)." );
        ImGui::DragFloat( "##SlowStreamingWarningFactor", &edit.SlowStreamingWarningFactor, 0.05f, 1.0f, 8.0f,
                          "%.2f" );
        commitOnEdit();
        endRow( "SlowStreamingWarningFactor" );

        Utils::ImGuiUtilities::BeginPropertyRow(
             "Max Concurrent Loads",
             "UE wp.Runtime.MaxLoadingStreamingCells: cell reads in flight at once (>= 1)." );
        const std::uint32_t step = 1;
        ImGui::InputScalar( "##MaxConcurrentLoads", ImGuiDataType_U32, &edit.MaxConcurrentLoads, &step, &step );
        commitOnEdit();
        endRow( "MaxConcurrentLoads" );

        Utils::ImGuiUtilities::BeginPropertyRow(
             "Activation Budget (ms)",
             "Main-thread time one frame may spend turning loaded cells into entities." );
        ImGui::InputDouble( "##ActivationBudgetMs", &edit.ActivationBudgetMs, 0.1, 1.0, "%.2f" );
        commitOnEdit();
        endRow( "ActivationBudgetMs" );

        Utils::ImGuiUtilities::BeginPropertyRow(
             "Unload Margin",
             "A loaded cell is kept until its source is this fraction of the loading range farther away (no "
             "flicker at the edge)." );
        ImGui::DragFloat( "##UnloadMargin", &edit.UnloadMargin, 0.01f, 0.0f, 4.0f, "%.2f" );
        commitOnEdit();
        endRow( "UnloadMargin" );

        Utils::ImGuiUtilities::BeginPropertyRow( "Retry Delay (s)",
                                                 "The wait before a failed cell read is retried." );
        ImGui::InputDouble( "##RetryDelaySeconds", &edit.RetryDelaySeconds, 0.1, 1.0, "%.2f" );
        commitOnEdit();
        endRow( "RetryDelaySeconds" );

        Utils::ImGuiUtilities::BeginPropertyRow( "Retry Backoff",
                                                 "Each consecutive failure multiplies the wait by this (>= 1)." );
        ImGui::InputDouble( "##RetryBackoff", &edit.RetryBackoff, 0.1, 1.0, "%.2f" );
        commitOnEdit();
        endRow( "RetryBackoff" );

        Utils::ImGuiUtilities::BeginPropertyRow( "Max Retry Delay (s)", "The longest wait between two retries." );
        ImGui::InputDouble( "##MaxRetryDelaySeconds", &edit.MaxRetryDelaySeconds, 1.0, 10.0, "%.1f" );
        commitOnEdit();
        endRow( "MaxRetryDelaySeconds" );
    }

    void ProjectSettingsPanel::CommitWorldPartition()
    {
        const auto saved = ::Desert::Project::SaveWorldPartitionSettings( m_EditWorldPartition );
        if ( saved )
            m_Problem.clear();
        else
            m_Problem = saved.GetError();
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
