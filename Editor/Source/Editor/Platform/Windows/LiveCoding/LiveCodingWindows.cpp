#include <Editor/LiveCoding/ILiveCodingBackend.hpp>
#include <Editor/LiveCoding/LiveCodingBuildIndex.hpp>

#include "CodeViewInlineSites.hpp"
#include "CoffObject.hpp"
#include "CompilerProcess.hpp"
#include "FunctionHotpatch.hpp"
#include "LinkerMap.hpp"
#include "PatchImage.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

// The PE/COFF backend: which sources a patch compiles, the compilers, and one in-memory link per loaded image.
namespace Desert::Editor::LiveCoding
{
    namespace
    {
        constexpr std::size_t kOutputTailLines = 40;

        /// The spelling CodeView::FindCarriers returns its files in: generic separators, ASCII lowercase.
        std::string Fold( const std::filesystem::path& path )
        {
            std::string text = path.generic_string();
            for ( char& c : text )
            {
                c = c == '\\' ? '/' : char( std::tolower( static_cast<unsigned char>( c ) ) );
            }
            return text;
        }

        /// The end of a compiler's output for a refusal, without the /showIncludes noise.
        std::string OutputTail( const std::string& output )
        {
            std::vector<std::string_view> lines;
            std::string_view              text( output );
            while ( !text.empty() )
            {
                const std::size_t end  = text.find( '\n' );
                std::string_view  line = text.substr( 0, end );
                text = end == std::string_view::npos ? std::string_view() : text.substr( end + 1 );
                if ( !line.empty() && line.back() == '\r' )
                {
                    line.remove_suffix( 1 );
                }
                if ( !line.empty() && !line.starts_with( "Note: including file" ) )
                {
                    lines.push_back( line );
                }
            }
            std::string tail;
            for ( std::size_t i = lines.size() > kOutputTailLines ? lines.size() - kOutputTailLines : 0;
                  i < lines.size(); ++i )
            {
                tail.append( lines[i] );
                tail.push_back( '\n' );
            }
            return tail;
        }

        /// Every module the process has loaded, the build's own images first: exports a patch may call.
        std::vector<std::uintptr_t> LoadedModules( std::span<const std::uintptr_t> first, std::uintptr_t except )
        {
            std::vector<std::uintptr_t> modules;
            for ( const std::uintptr_t base : first )
            {
                if ( base != 0 && base != except )
                {
                    modules.push_back( base );
                }
            }
            std::vector<HMODULE> handles( 1024 );
            DWORD                needed = 0;
            if ( K32EnumProcessModules( GetCurrentProcess(), handles.data(),
                                        DWORD( handles.size() * sizeof( HMODULE ) ), &needed ) )
            {
                handles.resize( std::min<std::size_t>( handles.size(), needed / sizeof( HMODULE ) ) );
                for ( const HMODULE handle : handles )
                {
                    const auto base = reinterpret_cast<std::uintptr_t>( handle );
                    if ( base != except && std::find( modules.begin(), modules.end(), base ) == modules.end() )
                    {
                        modules.push_back( base );
                    }
                }
            }
            return modules;
        }

        class PreparedPatch final : public IPreparedPatch
        {
        public:
            PreparedPatch( std::vector<PatchImage> images, PatchReport report )
                 : m_Images( std::move( images ) ), m_Report( std::move( report ) )
            {
            }

            Common::ResultStr<PatchReport> Apply() noexcept override
            {
                if ( m_Applied )
                {
                    return Common::MakeError<PatchReport>( "this patch was already applied" );
                }
                m_Applied        = true;
                const auto start = std::chrono::steady_clock::now();

                // Built before ApplyRedirects suspends anyone: nothing allocates while the threads are stopped.
                std::vector<Redirect> redirects;
                for ( const PatchImage& image : m_Images )
                {
                    const std::span<const Redirect> own = image.Redirects();
                    redirects.insert( redirects.end(), own.begin(), own.end() );
                }
                const Common::BoolResultStr redirected = ApplyRedirects( redirects );
                if ( !redirected )
                {
                    return Common::MakeError<PatchReport>( redirected.GetError() );
                }
                for ( const PatchImage& image : m_Images )
                {
                    for ( void ( *const initialiser )() : image.NewDataInitialisers() )
                    {
                        initialiser();
                    }
                }
                m_Report.ApplyMilliseconds =
                     std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
                PatchReport report = m_Report;
                return Common::MakeSuccess( std::move( report ) );
            }

        private:
            std::vector<PatchImage> m_Images;
            PatchReport             m_Report;
            bool                    m_Applied = false;
        };

        class WindowsBackend final : public ILiveCodingBackend
        {
        public:
            Common::ResultStr<std::unique_ptr<IPreparedPatch>>
            Prepare( const BuildIndex& index, std::span<const std::size_t> edited, std::atomic<PatchStage>& stage,
                     const std::atomic<bool>& cancel ) noexcept override;

        private:
            Common::ResultStr<const Coff::Object*> Original( const std::filesystem::path& path );
            Common::ResultStr<const LinkerMap*>    Map( const BuildIndex& index, std::size_t image );

            std::mutex                                                     m_Mutex;     // one Prepare at a time
            std::unordered_map<std::string, std::unique_ptr<Coff::Object>> m_Originals; // by BuiltObject path
            std::map<std::size_t, std::unique_ptr<LinkerMap>>              m_Maps;      // by image index
            std::uint32_t                                                  m_NextPatch = 0;
        };

        // The objects and maps the images were linked from do not change while they run (BuildIndex refuses a
        // build newer than its image), so each is read once per session.
        Common::ResultStr<const Coff::Object*> WindowsBackend::Original( const std::filesystem::path& path )
        {
            const std::string key   = Fold( path );
            const auto        known = m_Originals.find( key );
            if ( known != m_Originals.end() )
            {
                const Coff::Object* object = known->second.get();
                return Common::MakeSuccess( object );
            }
            Common::ResultStr<Coff::Object> loaded = Coff::Object::Load( path );
            if ( !loaded )
            {
                return Common::MakeError<const Coff::Object*>( loaded.GetError() );
            }
            auto                owned  = std::make_unique<Coff::Object>( loaded.ExtractValue() );
            const Coff::Object* object = owned.get();
            m_Originals.emplace( key, std::move( owned ) );
            return Common::MakeSuccess( object );
        }

        Common::ResultStr<const LinkerMap*> WindowsBackend::Map( const BuildIndex& index, std::size_t image )
        {
            const auto known = m_Maps.find( image );
            if ( known != m_Maps.end() )
            {
                const LinkerMap* map = known->second.get();
                return Common::MakeSuccess( map );
            }
            Common::ResultStr<LinkerMap> loaded = LinkerMap::Load( index.Images()[image].Map );
            if ( !loaded )
            {
                return Common::MakeError<const LinkerMap*>( loaded.GetError() );
            }
            auto             owned = std::make_unique<LinkerMap>( loaded.ExtractValue() );
            const LinkerMap* map   = owned.get();
            m_Maps.emplace( image, std::move( owned ) );
            return Common::MakeSuccess( map );
        }

        Common::ResultStr<std::unique_ptr<IPreparedPatch>>
        WindowsBackend::Prepare( const BuildIndex& index, std::span<const std::size_t> edited,
                                 std::atomic<PatchStage>& stage, const std::atomic<bool>& cancel ) noexcept
        {
            using Result = std::unique_ptr<IPreparedPatch>;
            std::lock_guard                     lock( m_Mutex );
            const std::span<const SourceRecord> sources = index.Sources();
            const std::span<const ImageRecord>  images  = index.Images();
            if ( edited.empty() )
            {
                return Common::MakeError<Result>( "no source changed since the image was built or last patched" );
            }

            // 1. Scanning: per image, the edited sources and every source whose functions inlined their code (in a
            //    unity batch a later member may inline an earlier one's function; those copies run the old code).
            stage.store( PatchStage::Scanning );
            std::map<std::size_t, std::vector<std::size_t>> compiledByImage; // image -> sources, edited first
            std::map<std::size_t, std::vector<std::size_t>> editedByImage;
            for ( const std::size_t source : edited )
            {
                if ( source >= sources.size() )
                {
                    return Common::MakeFormattedError<Result>( "source index {} is not in the build index",
                                                               source );
                }
                editedByImage[sources[source].Image].push_back( source );
            }
            std::unordered_map<std::string, std::size_t> sourceByFolded;
            for ( std::size_t i = 0; i < sources.size(); ++i )
            {
                sourceByFolded.emplace( Fold( sources[i].Source ), i );
            }
            // image -> the original carriers: what the image runs that holds code of the edited sources.
            std::map<std::size_t, std::unordered_set<std::string>> originalCarriers;
            for ( const auto& [image, members] : editedByImage )
            {
                std::vector<std::size_t>&          compiled = compiledByImage[image];
                std::vector<std::filesystem::path> editedPaths;
                std::map<std::string, std::filesystem::path>
                     builtObjects; // folded -> path, each unity object once
                for ( const std::size_t source : members )
                {
                    compiled.push_back( source );
                    editedPaths.push_back( sources[source].Source );
                    builtObjects.emplace( Fold( sources[source].BuiltObject ), sources[source].BuiltObject );
                }
                for ( const auto& [folded, path] : builtObjects )
                {
                    Common::ResultStr<const Coff::Object*> original = Original( path );
                    if ( !original )
                    {
                        return Common::MakeError<Result>( original.GetError() );
                    }
                    Common::ResultStr<CodeView::Carriers> carriers =
                         CodeView::FindCarriers( *original.GetValue(), editedPaths );
                    if ( !carriers )
                    {
                        return Common::MakeError<Result>( carriers.GetError() );
                    }
                    CodeView::Carriers found = carriers.ExtractValue();
                    for ( const std::string& name : found.Functions )
                    {
                        originalCarriers[image].insert( NormaliseAnonymousNamespace( name ) );
                    }
                    // A carrier owned by a source of the same image is recompiled with the edit; one owned by a
                    // header is matched by name after the compile (the unmatched check below refuses it).
                    for ( const std::filesystem::path& file : found.Files )
                    {
                        const auto inliner = sourceByFolded.find( Fold( file ) );
                        if ( inliner != sourceByFolded.end() && sources[inliner->second].Image == image &&
                             std::find( compiled.begin(), compiled.end(), inliner->second ) == compiled.end() )
                        {
                            compiled.push_back( inliner->second );
                        }
                    }
                }
            }

            // 2. Compiling: each source alone, with its translation unit's command, into its own object.
            stage.store( PatchStage::Compiling );
            const std::filesystem::path directory = index.PatchRoot() / std::to_string( m_NextPatch++ );
            std::error_code             error;
            std::filesystem::create_directories( directory, error );
            if ( error )
            {
                return Common::MakeFormattedError<Result>( "cannot create {}: {}", directory.generic_string(),
                                                           error.message() );
            }
            std::vector<CompileJob>                          jobs;
            std::vector<std::pair<std::size_t, std::size_t>> jobSource; // (image, source) per job
            for ( const auto& [image, compiled] : compiledByImage )
            {
                for ( const std::size_t source : compiled )
                {
                    const std::filesystem::path object =
                         directory /
                         ( std::to_string( jobs.size() ) + "_" + sources[source].Source.stem().string() + ".obj" );
                    jobs.push_back(
                         CompileJob{ index.CommandFor( source, object ), sources[source].Directory, object } );
                    jobSource.emplace_back( image, source );
                }
            }
            const auto     compileStart = std::chrono::steady_clock::now();
            const unsigned parallel     = std::max( 1u, std::thread::hardware_concurrency() / 4 );
            Common::ResultStr<std::vector<CompileResult>> ran = RunCompilers( jobs, parallel, cancel );
            if ( !ran )
            {
                return Common::MakeError<Result>( ran.GetError() );
            }
            const std::vector<CompileResult> results = ran.ExtractValue();
            for ( std::size_t i = 0; i < results.size(); ++i )
            {
                if ( results[i].ExitCode != 0 )
                {
                    return Common::MakeFormattedError<Result>(
                         "{} did not compile (exit {}):\n{}", sources[jobSource[i].second].Source.generic_string(),
                         results[i].ExitCode, OutputTail( results[i].Output ) );
                }
            }
            PatchReport report;
            report.CompileSeconds =
                 std::chrono::duration<double>( std::chrono::steady_clock::now() - compileStart ).count();

            // 3. Linking: per image, the new objects against the image as it runs.
            stage.store( PatchStage::Linking );
            std::vector<std::uintptr_t> imageBases( images.size(), 0 );
            for ( std::size_t i = 0; i < images.size(); ++i )
            {
                imageBases[i] =
                     reinterpret_cast<std::uintptr_t>( GetModuleHandleW( images[i].Image.wstring().c_str() ) );
            }
            std::vector<PatchImage> linked;
            std::size_t             job = 0;
            for ( const auto& [image, compiled] : compiledByImage )
            {
                if ( imageBases[image] == 0 )
                {
                    return Common::MakeFormattedError<Result>( "{} is not loaded in this process",
                                                               images[image].Image.generic_string() );
                }
                Common::ResultStr<const LinkerMap*> map = Map( index, image );
                if ( !map )
                {
                    return Common::MakeError<Result>( map.GetError() );
                }

                std::vector<Coff::Object>          objects;
                std::vector<std::string>           mapObjects;
                std::vector<const Coff::Object*>   originals;
                std::vector<std::filesystem::path> compiledPaths;
                for ( const std::size_t source : compiled )
                {
                    Common::ResultStr<Coff::Object> object = Coff::Object::Load( jobs[job].Object );
                    if ( !object )
                    {
                        return Common::MakeError<Result>( object.GetError() );
                    }
                    Common::ResultStr<const Coff::Object*> original = Original( sources[source].BuiltObject );
                    if ( !original )
                    {
                        return Common::MakeError<Result>( original.GetError() );
                    }
                    objects.push_back( object.ExtractValue() );
                    mapObjects.push_back( sources[source].BuiltObject.filename().string() );
                    originals.push_back( original.GetValue() );
                    compiledPaths.push_back( sources[source].Source );
                    ++job;
                }

                // What carries the edit in the new objects, and what carried it in the image: a function inlined
                // into before and compiled differently now is redirected all the same.
                std::unordered_set<std::string> carrying = originalCarriers[image];
                std::unordered_set<std::string> defined;
                for ( const Coff::Object& object : objects )
                {
                    Common::ResultStr<CodeView::Carriers> carriers =
                         CodeView::FindCarriers( object, compiledPaths );
                    if ( !carriers )
                    {
                        return Common::MakeError<Result>( carriers.GetError() );
                    }
                    const CodeView::Carriers& found = carriers.GetValue();
                    for ( const std::string& name : found.Functions )
                    {
                        carrying.insert( NormaliseAnonymousNamespace( name ) );
                    }
                    for ( const Coff::Symbol& symbol : object.Symbols() )
                    {
                        if ( symbol.Section > 0 && symbol.Function )
                        {
                            defined.insert( NormaliseAnonymousNamespace( symbol.Name ) );
                        }
                    }
                }
                // An image function that holds edited code the patch does not define again would keep running the
                // old code: a header function that inlined the edit, or a function the edit removed.
                std::string unmatched;
                std::size_t unmatchedCount = 0;
                for ( const std::string& name : originalCarriers[image] )
                {
                    const bool inImage =
                         std::any_of( mapObjects.begin(), mapObjects.end(), [&]( const std::string& o )
                                      { return map.GetValue()->Find( name, o ).has_value(); } );
                    if ( defined.count( name ) == 0 && inImage )
                    {
                        if ( unmatchedCount++ < 8 )
                        {
                            unmatched += ( unmatched.empty() ? "" : ", " ) + name;
                        }
                    }
                }
                if ( unmatchedCount != 0 )
                {
                    return Common::MakeFormattedError<Result>(
                         "{} function(s) of {} hold the edited code and are not in the patch ({}): a header "
                         "function "
                         "inlined the edit or the edit removed a function; rebuild and restart",
                         unmatchedCount, images[image].Target, unmatched );
                }

                const std::vector<std::uintptr_t> others = LoadedModules( imageBases, imageBases[image] );
                ImageSymbols                      symbols;
                symbols.Base                        = imageBases[image];
                symbols.Map                         = map.GetValue();
                symbols.MapObjects                  = mapObjects;
                symbols.Originals                   = originals;
                symbols.OtherImages                 = others;
                Common::ResultStr<PatchImage> patch = PatchImage::Link( objects, symbols, carrying, report );
                if ( !patch )
                {
                    return Common::MakeFormattedError<Result>( "{}: {}", images[image].Target, patch.GetError() );
                }
                linked.push_back( patch.ExtractValue() );
            }

            stage.store( PatchStage::Ready );
            Result prepared = std::make_unique<PreparedPatch>( std::move( linked ), std::move( report ) );
            return Common::MakeSuccess( std::move( prepared ) );
        }
    } // namespace

    Common::ResultStr<std::unique_ptr<ILiveCodingBackend>> CreateLiveCodingBackend()
    {
        std::unique_ptr<ILiveCodingBackend> backend = std::make_unique<WindowsBackend>();
        return Common::MakeSuccess( std::move( backend ) );
    }
} // namespace Desert::Editor::LiveCoding
