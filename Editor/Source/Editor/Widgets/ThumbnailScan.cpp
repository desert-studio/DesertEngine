#include "ThumbnailSweep.hpp"

#include <Editor/Import/CookPaths.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>

#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Editor
{
    // ────────────────────────────────────────────────────────────────────────────────────────────────
    // THE DECISION HALF OF THE SWEEP, IN A TRANSLATION UNIT WITH NO RENDERER IN IT.
    //
    // WHY THE CLASS IS SPLIT ACROSS TWO FILES, which is unusual here and therefore owes a reason. The
    // half in ThumbnailSweep.cpp calls `ThumbnailService`, whose header reaches AssetThumbnailRenderer
    // and from there the whole Vulkan renderer — so a suite cannot link it, and everything in it would
    // be provable only by launching the editor. The half in THIS file — which files need a picture, and
    // how many of them one frame may hand over — is the part with the arguable numbers in it, and the
    // lead asked for the sweep's budget to be asserted by a test rather than observed by attention.
    //
    // The split is along exactly that line: nothing here includes ThumbnailService.hpp, and
    // Desert/Tests/Editor/ThumbnailSweep compiles this file and nothing else of the editor.
    // ────────────────────────────────────────────────────────────────────────────────────────────────

    std::vector<ThumbnailSweepCandidate> ScanForMissingThumbnails( const std::filesystem::path& root,
                                                                   std::size_t                  limit )
    {
        std::vector<ThumbnailSweepCandidate> out;
        if ( root.empty() || limit == 0 )
            return out;

        // THE ONE ENUMERATION. `ListFilesRecursive` is what every content scanner in this engine uses,
        // including the preloader, and it is the only one that also sees a mounted `.dpak` — the font and
        // icon services each hand-rolled the disk half once and a packaged game scanned nothing.
        //
        // SAFE ON A WORKER **HERE**, and the qualification is the point. It consults the VFS mount stack,
        // which carries no lock; the stack is written by `VFS::MountPak`, whose only caller in this tree
        // is Runtime/Source/PackagedContent.cpp — the packaged game's startup. The editor never mounts a
        // pak, so in the process this sweep runs in the stack is empty and immutable for the session. If
        // an editor ever gains a mount, this call has to move or the VFS has to gain a lock.
        for ( const std::filesystem::path& file : Common::Utils::FileSystem::ListFilesRecursive( root ) )
        {
            if ( out.size() >= limit )
                break;

            const std::string path = file.generic_string();

            const ThumbnailFormats::Format* format =
                 ThumbnailFormats::Find( ThumbnailFormats::ExtensionOf( path ) );

            // Unknown to the browser, or a format whose picture is not generated: a decoded image is
            // already its own picture, an authored one is a person's decision, and a `None` row has a
            // written reason there must be no picture at all. All three are answers, and none of them is
            // work.
            if ( !format || !ThumbnailFormats::IsGenerated( format->By ) )
                continue;

            ThumbnailSweepCandidate candidate;
            candidate.AssetPath = path;
            candidate.By        = format->By;

            // The mesh is the one format whose picture is of a DIFFERENT file. A pure path computation
            // (fs::relative and a string replace, no stat), so it is free to do here; whether the cook
            // exists is asked at drain time, where the refusal can be reported with the manager's own
            // words rather than guessed at from a missing file.
            candidate.Subject = format->By == ThumbnailFormats::Producer::RenderedMesh
                                     ? CookPaths::MeshAsset( file ).generic_string()
                                     : path;

            candidate.Png = ThumbnailKey::DiskPath( candidate.Subject );

            // THE SAME QUESTION EVERY READER ASKS. Not `exists(png)`: that is the gate M8 found had
            // drifted away from the readers' rule, leaving assets that were neither drawn nor scheduled
            // for the life of the project.
            if ( ThumbnailFreshness::Judge( ThumbnailFreshness::Observe( candidate.Png, candidate.Subject ) ) !=
                 ThumbnailFreshness::Verdict::Capture )
                continue;

            out.push_back( std::move( candidate ) );
        }
        return out;
    }

    void ThumbnailSweeper::Reset()
    {
        // The future is left to finish on its own: it captures a path by value and touches nothing this
        // object owns, so abandoning it costs one worker a directory walk. Waiting instead would block
        // the frame in which a person changed project, to collect an answer about the project they left.
        m_Scan = {};
        m_Pending.clear();
        m_Next            = 0;
        m_FramesUntilScan = 0;
        m_OfferedThisPass = 0;
        m_Announced       = false;
        m_Offered.clear();
    }

    void ThumbnailSweeper::SetPendingForTest( std::vector<ThumbnailSweepCandidate> pending )
    {
        m_Pending = std::move( pending );
        m_Next    = 0;
    }

    int ThumbnailSweeper::Drain( const std::function<void( const ThumbnailSweepCandidate& )>& sink )
    {
        int handed = 0;
        while ( m_Next < m_Pending.size() && handed < kRequestsPerFrame )
        {
            sink( m_Pending[m_Next] );
            ++m_Next;
            ++handed;
        }
        if ( m_Next >= m_Pending.size() )
        {
            m_Pending.clear();
            m_Next = 0;
        }
        return handed;
    }

} // namespace Desert::Editor
