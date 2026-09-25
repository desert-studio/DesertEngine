#pragma once

#include <Editor/Core/EditorSubject.hpp>
#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Core/SubjectOpenRequest.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/AssetMetadata.hpp>

#include <cstdint>

namespace Desert::Editor::Core
{
    // "OPEN THIS ASSET" BY REFERENCE — the one route from an asset handle to its editor window, the shape of
    // UE's UAssetEditorSubsystem::OpenEditorForAsset: whoever holds a handle (a Details field, a path opener
    // that has just resolved a file, a palette entry) asks here, and the answer is either the subject that
    // the document well opens-or-focuses, or a refusal that names the handle and the type by number.
    //
    // WHY THE METADATA IS PASSED IN RATHER THAN LOOKED UP HERE. The lookup is one call on the AssetManager
    // (`FindMetadataByHandle`), and taking the manager instead would make every caller — and the suite that
    // pins this route — link the whole asset system to ask a question about three numbers. The caller does
    // the lookup and hands over what it found, `nullptr` included; the refusal for a handle the manager does
    // not know is made HERE, so it is worded once.
    //
    // WHY THE REGISTRY IS ASKED BEFORE QUEUEING. EditorLayer's drain would refuse a type with no editor as
    // well, but a frame later and to the log only; a caller that asked through this route (a script over
    // the control channel, a Details button) gets the refusal as its own answer, on the same call.
    [[nodiscard]] inline Common::ResultStr<SubjectId> AssetSubjectFor( const Assets::AssetMetadata* found,
                                                                       const Assets::AssetHandle&   requested,
                                                                       const SubjectEditorRegistry& editors )
    {
        if ( found == nullptr )
            return Common::MakeFormattedError<SubjectId>(
                 "asset {:016x} is not known to the asset manager — nothing to open",
                 static_cast<uint64_t>( requested ) );

        const auto type = static_cast<uint32_t>( found->AssetType );
        if ( found->AssetType == Assets::AssetTypeID::Unknown )
            return Common::MakeFormattedError<SubjectId>(
                 "asset {:016x} ('{}') has no type (AssetTypeID {}) — nothing can say which editor opens it",
                 static_cast<uint64_t>( requested ), found->Filepath.generic_string(), type );

        if ( !editors.HasEditorFor( AssetSubjectType( type ) ) )
            return Common::MakeFormattedError<SubjectId>(
                 "asset {:016x} ('{}') is of AssetTypeID {}, and no editor is registered for that type",
                 static_cast<uint64_t>( requested ), found->Filepath.generic_string(), type );

        return Common::MakeSuccess( AssetSubject( found->Handle, type ) );
    }

    // AssetSubjectFor, then the ordinary open-or-focus request. Two requests for the same asset in one frame
    // collapse inside SubjectOpenRequests::Request, so a double-click that also fires a single-click action
    // opens one window; a second request on a later frame reaches the document well, which FOCUSES the window
    // already open for that subject instead of building a second one.
    [[nodiscard]] inline Common::ResultStr<SubjectId> RequestOpenAsset( const Assets::AssetMetadata* found,
                                                                        const Assets::AssetHandle&   requested,
                                                                        const SubjectEditorRegistry& editors )
    {
        auto subject = AssetSubjectFor( found, requested, editors );
        if ( subject.IsSuccess() )
            SubjectOpenRequests::Request( subject.GetValue() );
        return subject;
    }
} // namespace Desert::Editor::Core
