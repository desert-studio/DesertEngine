#pragma once

// THE DESCRIPTOR INDEX OF A PARTITIONED WORLD (WP18): one EntityDescriptor (WorldPartitionRules.hpp) per entity
// file, kept beside the world, so the cook plans its cells without loading an entity - and WP19 can open a world
// in the editor unloaded and still draw where everything is.
//
// The pattern is UE's ActorDescContainer: descriptors are read out of the actors' packages once, cached, and a
// desc whose package changed is refreshed (ActorDescContainer.h, WorldPartitionActorDesc.h). Here each row
// carries the size and Crc32c of the entity file it was read from:
//   - Refresh re-describes exactly the entities whose file changed, drops the ones the world no longer lists and
//     writes the index; an unchanged file is not parsed;
//   - ReadFresh is THE GATE: an index whose row does not match its file (edited, deleted, added since) is refused
//     naming the file and the entity, never used stale.
//
// WHERE: `<scene dir>/__ExternalEntities__/<scene stem>/Descriptors.dedesc` - beside the entities it describes.
// It is derived: a missing index is built whole by Refresh, never by ReadFresh. Rows are in the order the world's
// header lists its entities, which is the order of the scene's records once joined - so descriptor `r` is the
// record `r` CookWorld plans.

#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Json/Json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core::DescriptorIndex
{
    inline constexpr std::string_view kFileName = "Descriptors.dedesc";

    struct DescriptorRow
    {
        std::uint64_t           Id    = 0;
        std::uint64_t           Bytes = 0; // the entity file's size
        std::uint32_t           Crc   = 0; // Crc32c of the entity file's bytes
        Rules::EntityDescriptor Descriptor;
    };

    struct DescriptorIndexFile
    {
        std::vector<DescriptorRow> Entities; // the header's list order
    };
    DESERT_JSON_STRUCT( DescriptorIndexFile, "EntityDescriptorIndex", 1 )

    // Where the index of the scene at `scenePath` lives.
    [[nodiscard]] std::filesystem::path PathOf( const std::filesystem::path& scenePath );

    // What a refresh did.
    struct RefreshOutcome
    {
        DescriptorIndexFile Index;
        std::size_t         Described = 0; // rows (re)read from their entity file
        std::size_t         Reused    = 0; // rows whose file had not changed
        std::size_t         Dropped   = 0; // rows of entities the world no longer lists
        bool                Written   = false;
    };

    // The text of entity `id`'s file; the default reads FileOf(scenePath, id).
    using RecordText = std::function<Common::ResultStr<std::string>( Common::UUID )>;

    // Brings the index of the partitioned world at `scenePath` up to `listed` (its header's entity list), reading
    // each entity's text through `textOf`, and writes it when it changed. A text that is not one entity record, or
    // states another id, is refused naming the entity. An index on disk that cannot be read is rebuilt whole.
    // `unchanged(id)` true (WP17's delta save) reuses that entity's previous row WITHOUT asking `textOf`; with no
    // previous row the entity is read as any other.
    using IsUnchanged = std::function<bool( Common::UUID )>;
    [[nodiscard]] Common::ResultStr<RefreshOutcome> Refresh( const std::filesystem::path&  scenePath,
                                                             std::span<const Common::UUID> listed,
                                                             const RecordText&             textOf,
                                                             const IsUnchanged&            unchanged = {} );

    // Refresh of the world as it is on disk: the list from its header, each text from its file.
    [[nodiscard]] Common::ResultStr<RefreshOutcome> Refresh( const std::filesystem::path& scenePath );

    // THE GATE: the index of the world at `scenePath`, only if every row matches the entity file it was read from
    // and the rows are exactly the header's list, in its order. Refused, naming the file: no index, an entity
    // the index lacks, a row of an entity the world no longer lists, a file whose size or checksum differs.
    [[nodiscard]] Common::ResultStr<DescriptorIndexFile> ReadFresh( const std::filesystem::path& scenePath );

    // The descriptors of `index`, in row order: what PlanWorldPartition and CookWorld take.
    [[nodiscard]] std::vector<Rules::EntityDescriptor> Descriptors( const DescriptorIndexFile& index );
} // namespace Desert::Core::DescriptorIndex
