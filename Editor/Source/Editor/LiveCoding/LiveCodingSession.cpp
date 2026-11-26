#include <Editor/LiveCoding/LiveCodingSession.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <format>
#include <system_error>
#include <utility>

namespace Desert::Editor::LiveCoding
{
    namespace
    {
        // "a, b, c": a list for the logged report.
        std::string Joined( const std::vector<std::string>& names )
        {
            std::string joined;
            for ( const std::string& name : names )
            {
                if ( !joined.empty() )
                    joined += ", ";
                joined += name;
            }
            return joined;
        }

        // THE MANIFEST CMAKE WRITES BESIDE Editor.exe: the build that made this process describes itself there.
        std::filesystem::path ManifestPath()
        {
            return Common::Utils::FileSystem::BaseDir() / "LiveCoding.json";
        }
    } // namespace

    LiveCodingSession::LiveCodingSession() : m_Backend( CreateLiveCodingBackend() )
    {
    }

    LiveCodingSession::~LiveCodingSession() noexcept
    {
        // The job reads m_Index and writes the members under m_Mutex: it ends before they do. It sets
        // m_JobDone and notifies under the lock, so taking the lock after the wait is its last touch of `this`.
        m_Cancel.store( true );
        m_JobDone.wait( false );
        const std::lock_guard lock( m_Mutex );
    }

    Common::BoolResultStr LiveCodingSession::RequestPatch()
    {
        const auto refuse = [this]( std::string reason ) -> Common::BoolResultStr
        {
            LOG_WARN( "[LiveCoding] refused: {}", reason );
            {
                const std::lock_guard lock( m_Mutex );
                m_Outcome = std::format( "refused: {}", reason );
            }
            return Common::MakeError<bool>( std::move( reason ) );
        };

        if ( !m_Backend )
            return refuse( m_Backend.GetError() );
        const PatchStage stage = m_Stage.load();
        if ( stage != PatchStage::Idle || !m_JobDone.load() )
            return refuse( std::format( "a patch is already running ({})",
                                        kPatchStageNames[static_cast<std::size_t>( stage )] ) );
        const std::filesystem::path manifest = ManifestPath();
        std::error_code             ec;
        if ( !std::filesystem::exists( manifest, ec ) )
            return refuse( std::format( "{} is missing: this editor was not built by CMake with Live Coding",
                                        manifest.string() ) );

        {
            const std::lock_guard lock( m_Mutex );
            m_Outcome.clear();
        }
        m_Cancel.store( false );
        m_JobDone.store( false );
        m_Stage.store( PatchStage::Scanning );

        const bool submitted = Common::JobSystem::Get().Submit(
             [this, manifest]() noexcept
             {
                 // A refusal ends the job with the reason as the outcome and the stage back at Idle.
                 const auto finish = [this]( std::string refusal )
                 {
                     const std::lock_guard lock( m_Mutex );
                     if ( !refusal.empty() )
                     {
                         LOG_WARN( "[LiveCoding] refused: {}", refusal );
                         m_Outcome = std::format( "refused: {}", refusal );
                         m_Stage.store( PatchStage::Idle );
                     }
                     m_JobDone.store( true );
                     m_JobDone.notify_all();
                 };

                 if ( !m_Index )
                 {
                     Common::ResultStr<BuildIndex> loaded = BuildIndex::Load( manifest );
                     if ( !loaded )
                         return finish( loaded.GetError() );
                     m_Index.emplace( loaded.ExtractValue() );
                 }

                 Common::ResultStr<std::vector<std::size_t>> changed = m_Index->ChangedSources();
                 if ( !changed )
                     return finish( changed.GetError() );
                 if ( changed.GetValue().empty() )
                 {
                     const std::lock_guard lock( m_Mutex );
                     LOG_INFO(
                          "[LiveCoding] nothing to patch: no source is newer than its image or its last patch" );
                     m_Outcome = "nothing to patch: no source is newer than its image or its last patch";
                     m_Stage.store( PatchStage::Idle );
                     m_JobDone.store( true );
                     m_JobDone.notify_all();
                     return;
                 }

                 // The mtimes BEFORE the compile: an edit saved while cl.exe runs is newer and patches next time.
                 std::vector<std::filesystem::file_time_type> at;
                 at.reserve( changed.GetValue().size() );
                 for ( const std::size_t source : changed.GetValue() )
                 {
                     std::error_code timeError;
                     at.push_back(
                          std::filesystem::last_write_time( m_Index->Sources()[source].Source, timeError ) );
                     if ( timeError )
                         return finish(
                              std::format( "source {} is gone", m_Index->Sources()[source].Source.string() ) );
                 }

                 Common::ResultStr<std::unique_ptr<IPreparedPatch>> prepared =
                      m_Backend.GetValue()->Prepare( *m_Index, changed.GetValue(), m_Stage, m_Cancel );
                 if ( !prepared )
                     return finish( prepared.GetError() );

                 const std::lock_guard lock( m_Mutex );
                 m_Prepared        = prepared.ExtractValue();
                 m_PreparedSources = changed.ExtractValue();
                 m_PreparedAt      = std::move( at );
                 m_Stage.store( PatchStage::Ready );
                 m_JobDone.store( true );
                 m_JobDone.notify_all();
             },
             Common::JobPriority::Background );
        if ( !submitted )
        {
            m_Stage.store( PatchStage::Idle );
            m_JobDone.store( true );
            return refuse( "the job system takes no more work (the editor is shutting down)" );
        }
        LOG_INFO( "[LiveCoding] patch started" );
        return Common::MakeSuccess( true );
    }

    void LiveCodingSession::ApplyPending()
    {
        if ( m_Stage.load() != PatchStage::Ready || !m_JobDone.load() )
            return;

        std::unique_ptr<IPreparedPatch>              prepared;
        std::vector<std::size_t>                     sources;
        std::vector<std::filesystem::file_time_type> at;
        {
            const std::lock_guard lock( m_Mutex );
            prepared = std::move( m_Prepared );
            sources  = std::move( m_PreparedSources );
            at       = std::move( m_PreparedAt );
        }

        // THE FRAME BOUNDARY: no frame code runs between the last frame and this call, so no thread is inside a
        // function whose entry the patch rewrites (the render thread joined the previous frame before OnUpdate).
        Common::ResultStr<PatchReport> applied = prepared->Apply();
        std::string                    outcome;
        if ( applied )
        {
            const PatchReport& report = applied.GetValue();
            m_Index->MarkPatched( sources, at );
            outcome = report.Summary();
            LOG_INFO( "[LiveCoding] {}", outcome );
            LOG_INFO( "[LiveCoding] redirected: {}", Joined( report.Redirected ) );
            if ( !report.NewFunctions.empty() )
                LOG_INFO( "[LiveCoding] new functions (no address in the image): {}",
                          Joined( report.NewFunctions ) );
            if ( !report.NewData.empty() )
                LOG_INFO( "[LiveCoding] new data: {}", Joined( report.NewData ) );
            if ( !report.NotRerun.empty() )
                LOG_WARN( "[LiveCoding] changed initialisers of existing data, in effect after a restart: {}",
                          Joined( report.NotRerun ) );
            LOG_INFO( "[LiveCoding] patch memory [{:#x}, {:#x})", report.PatchBegin, report.PatchEnd );
        }
        else
        {
            LOG_WARN( "[LiveCoding] refused: {}", applied.GetError() );
            outcome = std::format( "refused: {}", applied.GetError() );
        }
        {
            const std::lock_guard lock( m_Mutex );
            m_Outcome = std::move( outcome );
        }
        m_Stage.store( PatchStage::Idle );
    }

    Control::BackgroundReport LiveCodingSession::Report() const
    {
        const PatchStage          stage = m_Stage.load();
        Control::BackgroundReport report;
        report.Running  = stage != PatchStage::Idle;
        report.Progress = report.Running ? kPatchStageNames[static_cast<std::size_t>( stage )] : "";
        const std::lock_guard lock( m_Mutex );
        report.Outcome = m_Outcome;
        return report;
    }

    void LiveCodingSession::AppendCommands( std::vector<PaletteCommand>& out )
    {
        out.push_back( PaletteCommand{ .Group = "Live Coding", .Label = "Patch changed sources", .Run = [this]() {
                                          return RequestPatch();
                                      } } );
    }
} // namespace Desert::Editor::LiveCoding
