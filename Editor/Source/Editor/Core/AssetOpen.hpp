#pragma once

#include <Editor/Core/EditorSubject.hpp>
#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Core/SubjectOpenRequest.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/AssetMetadata.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

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
    // THE OPEN REGISTER: for EVERY Assets::AssetTypeID, whether "Open" has an editor window to go to, or the
    // refusal it answers with. `nullptr` means "opens" — EditorLayer registers a document editor for it, and
    // AssetOpenRoute's census reads EditorLayer.cpp to hold the two lists equal. The switch has no default,
    // so a new enumerator is a -Wswitch diagnostic here instead of an Open that quietly answers with the
    // generic "nothing registered"; the trailing return is reached only by a number no enumerator names.
    [[nodiscard]] constexpr const char* AssetOpenRefusal( const Assets::AssetTypeID type ) noexcept
    {
        constexpr const char* kNoEditor = "no document editor exists for this type";
        switch ( type )
        {
            case Assets::AssetTypeID::Material:
            case Assets::AssetTypeID::Texture2D:
            case Assets::AssetTypeID::CloudNoiseVolume:
            case Assets::AssetTypeID::CloudType:
            case Assets::AssetTypeID::CloudModellingVolume:
            case Assets::AssetTypeID::CloudLayout:
            case Assets::AssetTypeID::Skybox:
            case Assets::AssetTypeID::Mesh: // static meshes; a `.skmesh` is refused by name in AssetSubjectFor
                return nullptr;
            case Assets::AssetTypeID::Unknown:
                return "the asset has no type — nothing can say which editor opens it";
            case Assets::AssetTypeID::Shader:
            case Assets::AssetTypeID::Skeleton:
            case Assets::AssetTypeID::Animation:
            case Assets::AssetTypeID::Prefab:
            case Assets::AssetTypeID::UITheme:
            case Assets::AssetTypeID::StringTable:
            case Assets::AssetTypeID::ControlRig:
            case Assets::AssetTypeID::ShaderGraph:
            case Assets::AssetTypeID::AnimGraph:
            case Assets::AssetTypeID::Retarget:
                return kNoEditor;
            case Assets::AssetTypeID::Count:
                return "AssetTypeID::Count is the number of types, not a type";
        }
        return "the number is outside AssetTypeID";
    }

    [[nodiscard]] inline Common::ResultStr<SubjectId> AssetSubjectFor( const Assets::AssetMetadata* found,
                                                                       const Assets::AssetHandle&   requested,
                                                                       const SubjectEditorRegistry& editors )
    {
        if ( found == nullptr )
            return Common::MakeFormattedError<SubjectId>(
                 "asset {:016x} is not known to the asset manager — nothing to open",
                 static_cast<uint64_t>( requested ) );

        const auto type = static_cast<uint32_t>( found->AssetType );
        if ( const char* refusal = AssetOpenRefusal( found->AssetType ); refusal != nullptr )
            return Common::MakeFormattedError<SubjectId>(
                 "asset {:016x} ('{}') is a {} (AssetTypeID {}): {}", static_cast<uint64_t>( requested ),
                 found->Filepath.generic_string(), Assets::AssetTypeName( found->AssetType ), type, refusal );

        // ONE TYPE, TWO FORMATS: AssetTypeID::Mesh names both `.stmesh` and `.skmesh`, and only the static one
        // has a viewer (AV1f). The skinned one is refused here, by name, before a window is built for it.
        if ( found->AssetType == Assets::AssetTypeID::Mesh &&
             found->Filepath.extension() == Common::Constants::Extensions::SKINNED_MESH )
            return Common::MakeFormattedError<SubjectId>(
                 "asset {:016x} ('{}') is a skeletal mesh: only static meshes have a viewer; the skeletal mesh "
                 "viewer (AV1g) does not exist yet",
                 static_cast<uint64_t>( requested ), found->Filepath.generic_string() );

        // The register says it opens; an editor missing here is a wiring defect, not a property of the type.
        if ( !editors.HasEditorFor( AssetSubjectType( type ) ) )
            return Common::MakeFormattedError<SubjectId>(
                 "asset {:016x} ('{}') is a {} (AssetTypeID {}), which the open register says has an editor, "
                 "and none is registered",
                 static_cast<uint64_t>( requested ), found->Filepath.generic_string(),
                 Assets::AssetTypeName( found->AssetType ), type );

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
    // THE FOLDER "SHOW IN BROWSER" NAVIGATES TO. The asset's own directory, as the metadata records it — the
    // same folder a `run Browse <folder>` names, so both arrive through the one navigation EditorLayer owns.
    // A record with no file behind it (an in-memory asset nobody saved) has no folder, and says so by number.
    [[nodiscard]] inline Common::ResultStr<std::filesystem::path>
    AssetFolderFor( const Assets::AssetMetadata* found, const Assets::AssetHandle& requested )
    {
        if ( found == nullptr )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "asset {:016x} is not known to the asset manager — there is no folder to show",
                 static_cast<uint64_t>( requested ) );
        const std::filesystem::path folder = found->Filepath.parent_path();
        if ( folder.empty() )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "asset {:016x} (AssetTypeID {}) has no file on disk — there is no folder to show",
                 static_cast<uint64_t>( requested ), static_cast<uint32_t>( found->AssetType ) );
        return Common::MakeSuccess( folder );
    }

    // WHAT AN ASSET FIELD ASKED FOR. Details widgets hold a handle and nothing else: not the editor registry,
    // not the Assets browser, not the asset manager in a mutable form. So a field's "Open" / "Show in browser"
    // is QUEUED here and answered by EditorLayer, which owns all three, through RequestOpenAsset and the same
    // folder navigation `run Browse` uses — one route per action, whichever widget the click came from (a
    // reflected field, the Skybox picker, a mesh slot, the material pencil, a palette command).
    enum class AssetFieldAction : uint8_t
    {
        Open,
        ShowInBrowser,
    };

    struct AssetFieldRequest
    {
        Assets::AssetHandle Handle;
        AssetFieldAction    Action = AssetFieldAction::Open;

        [[nodiscard]] bool operator==( const AssetFieldRequest& other ) const
        {
            return Handle == other.Handle && Action == other.Action;
        }
    };

    class AssetFieldRequests
    {
    public:
        // The same collapse as SubjectOpenRequests: a double-click that also lands on the context menu's
        // item in one frame is one request.
        static void Request( const Assets::AssetHandle& handle, AssetFieldAction action )
        {
            const AssetFieldRequest request{ handle, action };
            auto&                   pending = Pending();
            for ( const auto& queued : pending )
            {
                if ( queued == request )
                    return;
            }
            pending.push_back( request );
        }

        [[nodiscard]] static bool HasPending()
        {
            return !Pending().empty();
        }

        static std::vector<AssetFieldRequest> Drain()
        {
            std::vector<AssetFieldRequest> drained;
            drained.swap( Pending() );
            return drained;
        }

    private:
        static std::vector<AssetFieldRequest>& Pending()
        {
            static std::vector<AssetFieldRequest> s_Pending;
            return s_Pending;
        }
    };
} // namespace Desert::Editor::Core
