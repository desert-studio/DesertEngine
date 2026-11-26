#include "DataAssetDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Panels/PropertyEditor/PropertyEditorBuilder.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/DataAsset.hpp>
#include <Engine/Assets/DataAssetTypeRegistry.hpp>
#include <Engine/Assets/Serialization/DataAsset.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Json/ReadWrite.hpp>

#include <ImGui/imgui.h>

#include <filesystem>
#include <format>
#include <memory>
#include <system_error>

namespace Desert::Editor
{
    namespace
    {
        std::shared_ptr<Assets::DataAsset> HeldAsset( Assets::AssetManager* assets, const Assets::AssetHandle& h )
        {
            return assets != nullptr ? assets->FindByHandle<Assets::DataAsset>( h ) : nullptr;
        }

        constexpr ImVec4 kRefusalColour( 1.0f, 0.4f, 0.4f, 1.0f );
    } // namespace

    DataAssetDocument::DataAssetDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument( AssetSubjectTitle( subject, assets, "Data Asset" ),
                             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::DataAsset ) ) ),
           m_Assets( assets )
    {
        const auto asset = HeldAsset( m_Assets, subject );
        if ( !asset )
        {
            m_LoadError = "the data asset is not held by the asset manager";
            return;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( auto loaded = asset->LoadFromFile(); !loaded )
            {
                m_LoadError = loaded.GetError();
                return;
            }
        }
        m_Edited    = asset->MutablePayload();
        m_SavedText = PayloadFingerprint();
    }

    DataAssetDocument::~DataAssetDocument()
    {
        // The builder's undo entries point at the payload this window edited.
        if ( m_Edited != nullptr )
            CommandHistory::Get().DropFor( m_Edited );
    }

    std::string DataAssetDocument::PayloadFingerprint() const
    {
        const auto asset = HeldAsset( m_Assets, Assets::AssetHandle( Subject().Owner ) );
        if ( !asset || asset->PayloadType() == nullptr || asset->MutablePayload() == nullptr )
            return {};
        const auto reflected = Assets::DataAssetTypeRegistry::ReflectedTypeOf( *asset->PayloadType() );
        if ( !reflected )
            return {};
        return Common::Json::Write(
             Reflection::SerializeReflected( *reflected.GetValue(), asset->MutablePayload() ) );
    }

    bool DataAssetDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    ISubjectDocument::DiskState DataAssetDocument::GetDiskState() const
    {
        if ( m_Edited == nullptr )
            return DiskState::Untracked;
        return PayloadFingerprint() != m_SavedText ? DiskState::Dirty : DiskState::Clean;
    }

    bool DataAssetDocument::SaveDocument()
    {
        const auto asset = HeldAsset( m_Assets, Assets::AssetHandle( Subject().Owner ) );
        if ( !asset || m_Edited == nullptr )
        {
            m_Refusal = "Save: the data asset is no longer held by the asset manager";
            return false;
        }
        if ( auto saved = asset->Save(); !saved )
        {
            m_Refusal = std::format( "Save: {}", saved.GetError() );
            LOG_ERROR( "[DataAsset] '{}' was not saved: {}", asset->GetMetadata().Filepath.string(),
                       saved.GetError() );
            return false;
        }
        m_SavedText = PayloadFingerprint();
        m_Refusal.clear();
        return true;
    }

    void DataAssetDocument::OnUIRender()
    {
        if ( !m_LoadError.empty() )
        {
            ImGui::TextColored( kRefusalColour, "Not loaded: %s", m_LoadError.c_str() );
            return;
        }
        const auto asset = HeldAsset( m_Assets, Assets::AssetHandle( Subject().Owner ) );
        if ( !asset || asset->PayloadType() == nullptr || asset->MutablePayload() == nullptr )
        {
            ImGui::TextUnformatted( "The data asset is no longer loaded." );
            return;
        }
        // A reload into ANOTHER type (the file was rewritten to hold another struct) makes a new payload: the
        // history's entries name the old one, so they are dropped and the new one is tracked. A reload of the
        // same type assigns in place (DataAsset::LoadFromFile) and keeps the pointer.
        if ( asset->MutablePayload() != m_Edited )
        {
            if ( m_Edited != nullptr )
                CommandHistory::Get().DropFor( m_Edited );
            m_Edited    = asset->MutablePayload();
            m_SavedText = PayloadFingerprint();
        }

        const auto reflected = Assets::DataAssetTypeRegistry::ReflectedTypeOf( *asset->PayloadType() );
        if ( !reflected )
        {
            ImGui::TextColored( kRefusalColour, "%s", reflected.GetError().c_str() );
            return;
        }

        ImGui::TextDisabled( "%s", asset->PayloadType()->Name.c_str() );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Save" ) )
            SaveDocument();
        if ( !m_Refusal.empty() )
            ImGui::TextColored( kRefusalColour, "%s", m_Refusal.c_str() );
        ImGui::Separator();

        // The builder's undo records are edits OF THE PAYLOAD (not of the scene): no scene file is dirtied, and
        // DropFor( m_Edited ) in the destructor forgets exactly them.
        const CommandHistory::FieldObject owner( m_Edited );
        PropertyEditorBuilder::Draw( asset->MutablePayload(), *reflected.GetValue(), m_Assets );
    }

    SubjectEditorRegistry::PathOpenOutcome RequestDataAssetDocument( Assets::AssetManager*        assets,
                                                                     const std::string&           path,
                                                                     const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr ||
             std::filesystem::path( path ).extension() != Assets::Serialization::kDataAssetExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::DataAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::DataAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[DataAsset] '{}' could not be registered as a data asset - no editor was opened.", path );
            return Outcome::Failed;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
            {
                LOG_ERROR( "[DataAsset] '{}' would not load - no editor was opened: {}", path, loaded.GetError() );
                return Outcome::Failed;
            }
        }

        const auto handle = asset->GetMetadata().Handle;
        if ( const auto opened = Core::RequestOpenAsset( assets->FindMetadataByHandle( handle ), handle, editors );
             !opened.IsSuccess() )
        {
            LOG_ERROR( "[DataAsset] '{}': {}", path, opened.GetError() );
            return Outcome::Failed;
        }
        return Outcome::Requested;
    }
} // namespace Desert::Editor
