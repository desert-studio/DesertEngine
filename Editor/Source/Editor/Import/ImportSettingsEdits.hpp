#pragma once
// THE DETAILS' IMPORT SETTINGS WORKING COPY, KEPT WITH ITS RECORD (THM1l-b11; UE: the Import Settings category
// edits the asset's UAssetImportData itself, and Reimport reads that same object - there is no second copy that
// can disagree with it). Ours is a file, `<source>.deimport`, which a person or a tool may rewrite while the
// editor runs; the copy the section edits is therefore read again whenever the record on disk is not the one it
// was read from, and Reimport takes the copy through the same door. Before this the section read the record once
// and kept its own copy for the session: a record edited on disk was overwritten from memory by the next Reimport.
#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>

#include <filesystem>

namespace Desert::Editor::ImportOptions
{
    struct ImportSettingsEdit
    {
        Assets::SourceImportSettings    Recorded;   // what the record stated when it was read
        Assets::SourceImportSettings    Edit;       // what the section shows and Reimport imports with
        Common::Content::ContentKind    Kind{};     // what the record says the source imports as (its fields)
        std::filesystem::file_time_type RecordTime; // the record's write time at that read
    };

    // @p source's working copy: read from its record on first ask and again whenever the record's write time
    // differs from the one it was read at (the record then wins over an edit not yet applied, being newer). An
    // error naming the record when it is missing or unreadable. The pointer lives until DropEdit(@p source).
    Common::ResultStr<ImportSettingsEdit*> EditOf( const std::filesystem::path& source );

    // Forgets @p source's copy (after a Reimport the record states what was imported; the next ask reads it).
    void DropEdit( const std::filesystem::path& source );
} // namespace Desert::Editor::ImportOptions
