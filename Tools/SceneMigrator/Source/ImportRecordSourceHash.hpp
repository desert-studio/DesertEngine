#pragma once

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace Desert::Migration
{
    // THE SKINNED IMPORT'S SOURCE HASH (SKEL-fixa; UE UAssetImportData's source file hash). SKEL-INT2 made a
    // skinned import current only when its source's `.deimport` states the hash of the bytes it read
    // (ImportRecordData::SourceHash). Every skinned import committed before that states none, so the editor
    // re-imported each of them at its first start and rewrote committed files. The committed outputs beside the
    // source ARE the import of the committed bytes, so this step states their hash: the record's text with
    // `SourceHash` set, a new SkinnedMesh record when the source has none yet. A SkinnedMesh record states its
    // mesh's `Bounds` (the reader refuses one without), so a record that lacks them takes the box of
    // @p skinnedMesh, the committed import output (already in the engine's space). nullopt = nothing to state
    // (the record states a hash and its box - equal or not, a stated hash is the import's own word and is left
    // as it is). @p source is the raw mesh source beside @p skinnedMesh (Common::Content::MeshSourceBeside).
    Common::ResultStr<std::optional<std::string>>
    ImportRecordWithSourceHash( const std::filesystem::path& source, const std::filesystem::path& skinnedMesh );
} // namespace Desert::Migration
