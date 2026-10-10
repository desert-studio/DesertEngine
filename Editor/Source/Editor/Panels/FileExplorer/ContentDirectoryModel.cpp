#include "ContentDirectoryModel.hpp"

#include <Editor/Panels/FileExplorer/ContentBrowserUtils.hpp>
#include <Editor/Panels/FileExplorer/FileTypeInfo.hpp>
#include <Common/Content/ContentScan.hpp>

#include <functional>
#include <system_error>

namespace Desert::Editor
{
    ContentDirectoryModel::ContentDirectoryModel( const std::string& rootPath )
         : m_RootPath( rootPath.empty() ? std::string( "Assets" ) : rootPath )
    {
        m_Root = Find( Process( m_RootPath, nullptr, true ) );
    }

    DirectoryInformation* ContentDirectoryModel::Find( const std::string& path ) const
    {
        const auto it = m_Directories.find( path );
        return it != m_Directories.end() ? it->second.get() : nullptr;
    }

    void ContentDirectoryModel::Open( DirectoryInformation* dir )
    {
        if ( dir != nullptr && !dir->Opened )
            Process( dir->AssetPath, dir->Parent, true );
    }

    void ContentDirectoryModel::Rescan( DirectoryInformation* dir )
    {
        if ( dir == nullptr )
            return;
        for ( auto* child : dir->Children )
            if ( child != nullptr )
                m_Directories.erase( child->AssetPath );
        dir->Children.clear();
        dir->Opened = false;
        Process( dir->AssetPath, dir->Parent, true );
    }

    bool ContentDirectoryModel::Shows( const std::filesystem::path& path ) const
    {
        return m_ShowHiddenFiles || !ContentBrowserUtils::IsHidden( path );
    }

    std::vector<std::string> ContentDirectoryModel::AllFolders() const
    {
        std::vector<std::string> folders;
        if ( m_Root == nullptr )
            return folders;
        folders.push_back( m_Root->AssetPath );
        std::error_code ec;
        for ( auto it = std::filesystem::recursive_directory_iterator( m_Root->AssetPath, ec );
              !ec && it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
        {
            if ( !Shows( it->path() ) )
            {
                it.disable_recursion_pending(); // the tree never opens what it hides
                continue;
            }
            if ( it->is_directory( ec ) )
                folders.push_back( it->path().generic_string() );
        }
        return folders;
    }

    std::vector<std::string> ContentDirectoryModel::AllFiles() const
    {
        std::vector<std::string> files;
        if ( m_Root == nullptr )
            return files;
        std::error_code ec;
        for ( auto it = std::filesystem::recursive_directory_iterator( m_Root->AssetPath, ec );
              !ec && it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
        {
            if ( !Shows( it->path() ) )
            {
                it.disable_recursion_pending();
                continue;
            }
            if ( it->is_regular_file( ec ) )
                files.push_back( it->path().generic_string() );
        }
        return files;
    }

    // A directory tree is walked by its own depth, which the file system bounds.
    // NOLINTNEXTLINE(misc-no-recursion)
    std::string ContentDirectoryModel::Process( const std::string& directoryPath, DirectoryInformation* parent,
                                                bool processChildren )
    {
        const auto& directory = m_Directories[directoryPath];
        if ( directory && directory->Opened )
            return directory->AssetPath;

        // The path AS THE CALLER SPELLED IT is the node's identity: it is the key in m_Directories, the string
        // the navigation history stores, and what every child is built from. The model is DISK-shaped, which is
        // what its row in Tests/Common/ContentScanners records with the measurement behind it.
        const std::filesystem::path stdPath( directoryPath );

        const std::shared_ptr<DirectoryInformation> directoryInfo =
             directory ? directory
                       : std::make_shared<DirectoryInformation>( directoryPath,
                                                                 !std::filesystem::is_directory( stdPath ) );
        directoryInfo->Parent    = parent;
        directoryInfo->AssetPath = directoryPath;

        std::string extension = stdPath.extension().string();
        if ( !extension.empty() && extension[0] == '.' )
            extension = extension.substr( 1 );

        if ( std::filesystem::is_directory( stdPath ) )
        {
            directoryInfo->IsFile = false;
            directoryInfo->Leaf   = true;
            for ( const auto& entry : std::filesystem::directory_iterator( stdPath ) )
            {
                if ( !Shows( entry.path() ) )
                    continue;

                if ( entry.is_directory() )
                    directoryInfo->Leaf = false;

                if ( processChildren )
                {
                    directoryInfo->Opened = true;

                    const std::string subdirHandle =
                         Process( entry.path().generic_string(), directoryInfo.get(), false );
                    directoryInfo->Children.push_back( m_Directories[subdirHandle].get() );
                }
            }
        }
        else
        {
            // Root-aware: a `.detex` under the Skybox root is a Skybox, not a Texture (FileTypeOfContent).
            const FileType fileType =
                 FileTypeOfContent( extension, Common::Content::KindOfContentFile( stdPath ) );

            directoryInfo->IsFile = true;
            directoryInfo->Type   = fileType;
            directoryInfo->FileSize =
                 std::filesystem::exists( stdPath ) ? std::filesystem::file_size( stdPath ) : 0;
            {
                std::error_code wec;
                const auto      t            = std::filesystem::last_write_time( stdPath, wec );
                directoryInfo->LastWriteTime = wec ? 0 : static_cast<uint64_t>( t.time_since_epoch().count() );
            }
            directoryInfo->Hidden =
                 std::filesystem::exists( stdPath ) ? ContentBrowserUtils::IsHidden( stdPath ) : true;
            directoryInfo->Opened         = true;
            directoryInfo->Leaf           = true;
            directoryInfo->FileTypeColour = FileTypeInfoOf( fileType ).Colour;
        }

        if ( !directory )
            m_Directories[directoryInfo->AssetPath] = directoryInfo;
        return directoryInfo->AssetPath;
    }

    namespace
    {
        // Cheap signature of a directory's immediate entries (name + write-time). Detects external
        // add/remove/rename without an OS watch API.
        std::size_t DirectorySignature( const std::string& dirPath )
        {
            std::size_t     sig = 0;
            std::error_code ec;
            if ( !std::filesystem::is_directory( dirPath, ec ) )
                return sig;
            for ( const auto& entry : std::filesystem::directory_iterator( dirPath, ec ) )
            {
                if ( ec )
                    break;
                sig ^= std::hash<std::string>{}( entry.path().filename().string() ) + 0x9e3779b9 + ( sig << 6 ) +
                       ( sig >> 2 );
                std::error_code tec;
                const auto      t = std::filesystem::last_write_time( entry.path(), tec );
                if ( !tec )
                    sig ^= static_cast<std::size_t>( t.time_since_epoch().count() ) + 0x9e3779b9 + ( sig << 6 ) +
                           ( sig >> 2 );
            }
            return sig;
        }
    } // namespace

    bool DirectoryWatcher::Poll( const std::string& dirPath )
    {
        if ( ++m_PollCounter < kPollFrames )
            return false;
        m_PollCounter = 0;

        const std::size_t sig = DirectorySignature( dirPath );
        if ( sig == m_Signature )
            return false;
        m_Signature = sig;
        return true;
    }

    void DirectoryWatcher::Rebase( const std::string& dirPath )
    {
        m_Signature   = DirectorySignature( dirPath );
        m_PollCounter = 0;
    }
} // namespace Desert::Editor
