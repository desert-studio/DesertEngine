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
    // `SourceHash` set, a new SkinnedMesh record when the source has none yet. nullopt = nothing to state (the
    // record already states a hash - equal or not, a stated hash is the import's own word and is left as it is).
    // @p source is the raw mesh source beside a `.skmesh` (Common::Content::MeshSourceBeside).
    Common::ResultStr<std::optional<std::string>>
    ImportRecordWithSourceHash( const std::filesystem::path& source );
} // namespace Desert::Migration
