#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>

#include <Engine/Core/Serialize/ExternalEntities.hpp>

#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <system_error>
#include <unordered_map>

namespace Desert::Core::DescriptorIndex
{
    namespace
    {
        namespace fs = std::filesystem;

        std::uint32_t CrcOf( const std::string& text )
        {
            return Common::Utils::Crc32c( text.data(), text.size() );
        }

        Common::ResultStr<std::vector<Common::UUID>> ListedEntities( const fs::path& scenePath )
        {
            return ExternalEntities::ListedEntities( scenePath );
        }

        Common::ResultStr<std::string> FileText( const fs::path& scenePath, Common::UUID id )
        {
            const fs::path  file = ExternalEntities::FileOf( scenePath, id );
            std::error_code ec;
            if ( !fs::is_regular_file( file, ec ) )
                return Common::MakeError<std::string>( fmt::format( "its file {} does not exist", file.string() ) );
            return Common::Utils::FileSystem::ReadFileContent( file );
        }
    } // namespace

    fs::path PathOf( const fs::path& scenePath )
    {
        return ExternalEntities::DirectoryOf( scenePath ) / kFileName;
    }

    Common::ResultStr<RefreshOutcome> Refresh( const fs::path& scenePath, std::span<const Common::UUID> listed,
                                               const RecordText& textOf )
    {
        RefreshOutcome outcome;
        const fs::path indexPath = PathOf( scenePath );

        // The previous rows by id. Derived data: one that cannot be read is rebuilt, and nothing of it is used.
        DescriptorIndexFile previous;
        std::error_code     ec;
        if ( fs::is_regular_file( indexPath, ec ) )
            if ( auto read = Common::Json::ReadFile<DescriptorIndexFile>( indexPath ) )
                previous = read.ExtractValue();
        std::unordered_map<std::uint64_t, const DescriptorRow*> before;
        for ( const DescriptorRow& row : previous.Entities )
            before.emplace( row.Id, &row );

        for ( const Common::UUID id : listed )
        {
            const auto bits = static_cast<std::uint64_t>( id );
            auto       text = textOf( id );
            if ( !text )
                return Common::MakeError<RefreshOutcome>(
                     fmt::format( "'{}': entity {}: {}", scenePath.string(), bits, text.GetError() ) );
            DescriptorRow row;
            row.Id    = bits;
            row.Bytes = text.GetValue().size();
            row.Crc   = CrcOf( text.GetValue() );
            if ( const auto found = before.find( bits ); found != before.end() &&
                                                         found->second->Bytes == row.Bytes &&
                                                         found->second->Crc == row.Crc )
            {
                row.Descriptor = found->second->Descriptor;
                ++outcome.Reused;
            }
            else
            {
                auto record = Common::Json::Read<Assets::EntityData>( text.GetValue() );
                if ( !record )
                    return Common::MakeError<RefreshOutcome>( fmt::format(
                         "'{}': entity {} is not an entity record: {}", scenePath.string(), bits, record.GetError() ) );
                row.Descriptor = Rules::DescribeEntity( record.GetValue() );
                if ( row.Descriptor.Id != bits )
                    return Common::MakeError<RefreshOutcome>( fmt::format(
                         "'{}': the file of entity {} states another id", scenePath.string(), bits ) );
                ++outcome.Described;
            }
            outcome.Index.Entities.push_back( std::move( row ) );
        }
        outcome.Dropped = previous.Entities.size() > outcome.Reused ? previous.Entities.size() - outcome.Reused : 0;

        const std::string text = Common::Json::Write( outcome.Index );
        const auto        old  = fs::is_regular_file( indexPath, ec )
                                      ? Common::Utils::FileSystem::ReadFileContent( indexPath )
                                      : Common::ResultStr<std::string>( Common::MakeError<std::string>( "none" ) );
        if ( !old || old.GetValue() != text )
        {
            fs::create_directories( indexPath.parent_path(), ec );
            if ( const auto written = Common::Json::WriteFileAtomic( indexPath, outcome.Index ); !written )
                return Common::MakeError<RefreshOutcome>(
                     fmt::format( "could not write {}: {}", indexPath.string(), written.GetError() ) );
            outcome.Written = true;
        }
        return Common::MakeSuccess( std::move( outcome ) );
    }

    Common::ResultStr<RefreshOutcome> Refresh( const fs::path& scenePath )
    {
        auto listed = ListedEntities( scenePath );
        if ( !listed )
            return Common::MakeError<RefreshOutcome>( listed.GetError() );
        return Refresh( scenePath, listed.GetValue(),
                        [&]( Common::UUID id ) { return FileText( scenePath, id ); } );
    }

    Common::ResultStr<DescriptorIndexFile> ReadFresh( const fs::path& scenePath )
    {
        using Result         = DescriptorIndexFile;
        const fs::path index = PathOf( scenePath );
        std::error_code ec;
        if ( !fs::is_regular_file( index, ec ) )
            return Common::MakeError<Result>( fmt::format(
                 "'{}' has no descriptor index ({}); saving the world or cooking it builds one", scenePath.string(),
                 index.string() ) );
        auto read = Common::Json::ReadFile<DescriptorIndexFile>( index );
        if ( !read )
            return Common::MakeError<Result>( read.GetError() );
        auto listed = ListedEntities( scenePath );
        if ( !listed )
            return Common::MakeError<Result>( listed.GetError() );

        const auto& rows = read.GetValue().Entities;
        for ( std::size_t at = 0; at < std::max( rows.size(), listed.GetValue().size() ); ++at )
        {
            if ( at >= rows.size() )
                return Common::MakeError<Result>(
                     fmt::format( "{} is stale: entity {} is not in it", index.string(),
                                  static_cast<std::uint64_t>( listed.GetValue()[at] ) ) );
            if ( at >= listed.GetValue().size() || static_cast<std::uint64_t>( listed.GetValue()[at] ) != rows[at].Id )
                return Common::MakeError<Result>(
                     fmt::format( "{} is stale: it describes entity {}, which '{}' does not list there",
                                  index.string(), rows[at].Id, scenePath.string() ) );
            const fs::path file = ExternalEntities::FileOf( scenePath, Common::UUID( rows[at].Id ) );
            auto           text = FileText( scenePath, Common::UUID( rows[at].Id ) );
            if ( !text )
                return Common::MakeError<Result>( fmt::format( "{} is stale: entity {}: {}", index.string(),
                                                               rows[at].Id, text.GetError() ) );
            if ( text.GetValue().size() != rows[at].Bytes || CrcOf( text.GetValue() ) != rows[at].Crc )
                return Common::MakeError<Result>(
                     fmt::format( "{} is stale: {} changed since entity {} was described", index.string(),
                                  file.string(), rows[at].Id ) );
        }
        return read;
    }

    std::vector<Rules::EntityDescriptor> Descriptors( const DescriptorIndexFile& index )
    {
        std::vector<Rules::EntityDescriptor> out;
        out.reserve( index.Entities.size() );
        for ( const DescriptorRow& row : index.Entities )
            out.push_back( row.Descriptor );
        return out;
    }
} // namespace Desert::Core::DescriptorIndex
