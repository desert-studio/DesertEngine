// AV1c: a Details field's "Open" and a browser double-click reach ONE subject, and the Material Editor's
// registration — not the route that asked — loads the material, so a by-handle open of a record works.

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Panels/MaterialEditor/MaterialDocumentOpen.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

using namespace Desert;
using namespace Desert::Editor;

namespace
{
    // The registrations EditorLayer makes, by type number: which types exist is what matters here, since the
    // route asks HasEditorFor and nothing else of a registration.
    SubjectEditorRegistry RegistryOfEditorLayerTypes()
    {
        SubjectEditorRegistry editors;
        for ( const auto type : { Assets::AssetTypeID::Material, Assets::AssetTypeID::Texture2D,
                                  Assets::AssetTypeID::CloudNoiseVolume, Assets::AssetTypeID::CloudType,
                                  Assets::AssetTypeID::CloudModellingVolume, Assets::AssetTypeID::CloudLayout } )
        {
            editors.Register( AssetSubjectType( static_cast<uint32_t>( type ) ),
                              SubjectEditorRegistry::Registration{
                                   "T", "", []( const SubjectId& ) { return std::unique_ptr<ISubjectDocument>(); },
                                   []( const SubjectId& ) { return true; } } );
        }
        return editors;
    }

    // What EditorLayer::ServiceSubjectOpenRequests does with a field's request — the drain, on the pure route.
    std::vector<SubjectId> DrainFieldRequests( const Assets::AssetManager& manager,
                                               const SubjectEditorRegistry& editors )
    {
        for ( const auto& request : Editor::Core::AssetFieldRequests::Drain() )
            (void)Editor::Core::RequestOpenAsset( manager.FindMetadataByHandle( request.Handle ), request.Handle, editors );
        return Editor::Core::SubjectOpenRequests::Drain();
    }

    struct TempMaterial
    {
        std::filesystem::path Dir  = std::filesystem::temp_directory_path() / "desert_material_document_open";
        std::filesystem::path File = Dir / "av1c_material.demat";
        TempMaterial()
        {
            std::filesystem::create_directories( Dir );
        }
        ~TempMaterial()
        {
            std::error_code ec;
            std::filesystem::remove_all( Dir, ec );
        }
    };
} // namespace

TEST( MaterialDocumentOpen, AFieldAndABrowserDoubleClickReachTheSameSubject )
{
    TempMaterial          tmp;
    Assets::AssetManager  manager;
    SubjectEditorRegistry editors = RegistryOfEditorLayerTypes();
    const auto            written = Assets::WriteMaterialFile( tmp.File, Assets::MaterialData{} );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();

    (void)Editor::Core::SubjectOpenRequests::Drain();
    ASSERT_EQ( RequestMaterialDocument( &manager, tmp.File.generic_string(), editors ),
               MaterialDocumentRequest::Requested );
    const auto browser = Editor::Core::SubjectOpenRequests::Drain();
    ASSERT_EQ( browser.size(), 1U );

    const auto record = manager.FindByPath<Assets::SurfaceMaterialAsset>( tmp.File.generic_string() );
    if ( !record )
    {
        ADD_FAILURE() << "the browser route made no record";
        return;
    }
    Editor::Core::AssetFieldRequests::Request( record->GetMetadata().Handle, Editor::Core::AssetFieldAction::Open );
    const auto field = DrainFieldRequests( manager, editors );
    ASSERT_EQ( field.size(), 1U );
    EXPECT_EQ( field.front(), browser.front() );
    EXPECT_EQ( field.front(),
               AssetSubject( record->GetMetadata().Handle, static_cast<uint32_t>( Assets::AssetTypeID::Material ) ) );
}

TEST( MaterialDocumentOpen, EveryRegisteredTypeOpensFromAFieldAsItsOwnSubject )
{
    const SubjectEditorRegistry editors = RegistryOfEditorLayerTypes();
    uint64_t                    next    = 0xA1C0;
    for ( const auto type : { Assets::AssetTypeID::Material, Assets::AssetTypeID::Texture2D,
                              Assets::AssetTypeID::CloudNoiseVolume, Assets::AssetTypeID::CloudType,
                              Assets::AssetTypeID::CloudModellingVolume, Assets::AssetTypeID::CloudLayout } )
    {
        Assets::AssetMetadata meta;
        meta.Handle    = Assets::AssetHandle( ++next );
        meta.AssetType = type;
        meta.Filepath  = "Assets/x.bin";
        // A field queues; the drain asks the route with the metadata the manager holds — the same call the
        // browser's path openers end in (RequestMaterialDocument above, RequestTextureDocument, the clouds).
        Editor::Core::AssetFieldRequests::Request( meta.Handle, Editor::Core::AssetFieldAction::Open );
        for ( const auto& request : Editor::Core::AssetFieldRequests::Drain() )
            (void)Editor::Core::RequestOpenAsset( &meta, request.Handle, editors );
        const auto opened = Editor::Core::SubjectOpenRequests::Drain();
        ASSERT_EQ( opened.size(), 1U ) << "AssetTypeID " << static_cast<uint32_t>( type );
        EXPECT_EQ( opened.front(), AssetSubject( meta.Handle, static_cast<uint32_t>( type ) ) );
    }
}

TEST( MaterialDocumentOpen, ARecordIsLoadedByTheEditorsOwnPreparationNotByTheRoute )
{
    TempMaterial         tmp;
    Assets::AssetManager manager;
    const auto           written = Assets::WriteMaterialFile( tmp.File, Assets::MaterialData{} );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();

    // A record only — what a scene's material slot names before anything has drawn it.
    auto record = manager.CreateAsset<Assets::SurfaceMaterialAsset>( Assets::AssetPriority::Medium,
                                                                    Common::Filepath( tmp.File ), false );
    if ( !record )
    {
        ADD_FAILURE() << "no record";
        return;
    }
    ASSERT_FALSE( record->IsReadyForUse() );

    const auto ready = EnsureMaterialLoaded( manager, record->GetMetadata().Handle );
    ASSERT_TRUE( ready.IsSuccess() ) << ready.GetError();
    EXPECT_TRUE( ready.GetValue()->IsReadyForUse() );
}

TEST( MaterialDocumentOpen, AnUnknownHandleIsRefusedByNumber )
{
    Assets::AssetManager manager;
    const auto           ready = EnsureMaterialLoaded( manager, Assets::AssetHandle( 0xBADC0DEULL ) );
    ASSERT_FALSE( ready.IsSuccess() );
    EXPECT_NE( ready.GetError().find( "000000000badc0de" ), std::string::npos ) << ready.GetError();

    const auto folder = Editor::Core::AssetFolderFor( nullptr, Assets::AssetHandle( 0xBADC0DEULL ) );
    ASSERT_FALSE( folder.IsSuccess() );
    EXPECT_NE( folder.GetError().find( "000000000badc0de" ), std::string::npos );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
