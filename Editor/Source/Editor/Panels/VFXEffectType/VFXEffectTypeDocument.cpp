#include "VFXEffectTypeDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/SubjectTitle.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/VFXEffectTypeAsset.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <system_error>
#include <utility>

namespace Desert::Editor
{
    namespace
    {
        using Levels   = std::vector<Assets::Serialization::VFXEffectTypeLevel>;
        using Authored = VFXEffectTypeAuthored;

        Authored AuthoredOf( const Assets::Serialization::VFXEffectTypeData& data )
        {
            return { data.CullReaction, data.Significance, data.Levels };
        }

        void Assign( Assets::Serialization::VFXEffectTypeData& data, const Authored& authored )
        {
            data.CullReaction = authored.CullReaction;
            data.Significance = authored.Significance;
            data.Levels       = authored.Levels;
        }

        // UE ENiagaraCullReaction / the significance handlers, in enum order.
        constexpr std::array<const char*, 4> kReactionNames = {
             "Deactivate", "Deactivate Immediate", "Deactivate Resume", "Deactivate Immediate Resume" };
        constexpr std::array<const char*, 2> kSignificanceNames = { "Distance", "Age" };

        // The Effects.QualityLevel names, Low .. Cinematic (Scalability.json's Effects rows).
        constexpr std::array<const char*, Assets::Serialization::kVFXEffectTypeLevelCount> kLevelNames = {
             "Low", "Medium", "High", "Epic", "Cinematic" };

        std::string DescribeLevels( const Levels& levels )
        {
            std::string text;
            for ( const auto& level : levels )
                text += std::format( "{}{:.0f} cm x{:.2f} max {}/{}{} {:.1f} s", text.empty() ? "" : ", ",
                                     level.MaxDistance, level.SpawnCountScale, level.MaxInstances,
                                     level.MaxSystemInstances, level.CullByViewFrustum ? " frustum" : "",
                                     level.MaxTimeWithoutRender );
            return text;
        }

        /// One undo step of a type: everything the window edits, before and after.
        class VFXEffectTypeEditCommand final : public ICommand
        {
        public:
            VFXEffectTypeEditCommand( Assets::Serialization::VFXEffectTypeData* target, Authored before,
                                      Authored after, std::string label )
                 : m_Target( target ), m_Before( std::move( before ) ), m_After( std::move( after ) ),
                   m_Label( std::move( label ) )
            {
            }

            bool Undo() override
            {
                Assign( *m_Target, m_Before );
                return true;
            }

            bool Redo() override
            {
                Assign( *m_Target, m_After );
                return true;
            }

            [[nodiscard]] const void* EditedObject() const override
            {
                return m_Target;
            }

            std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            Assets::Serialization::VFXEffectTypeData* m_Target;
            Authored                                  m_Before;
            Authored                                  m_After;
            std::string                               m_Label;
        };
    } // namespace

    VFXEffectTypeDocument::VFXEffectTypeDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument(
                AssetSubjectTitle( subject, assets, "VFX Effect Type" ),
                AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::VFXEffectType ) ) ),
           m_Assets( assets )
    {
        const auto asset =
             m_Assets != nullptr ? m_Assets->FindByHandle<Assets::VFXEffectTypeAsset>( subject ) : nullptr;
        if ( !asset )
        {
            m_LoadError = "the effect type asset is not held by the asset manager";
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
        m_Working = asset->GetData();
        m_Saved   = m_Working;
        m_Loaded  = true;
    }

    VFXEffectTypeDocument::~VFXEffectTypeDocument()
    {
        // The history's entries point at m_Working, which dies with this window.
        CommandHistory::Get().DropFor( &m_Working );
    }

    bool VFXEffectTypeDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    ISubjectDocument::DiskState VFXEffectTypeDocument::GetDiskState() const
    {
        if ( !m_Loaded )
            return DiskState::Untracked;
        return AuthoredOf( m_Working ) != AuthoredOf( m_Saved ) ? DiskState::Dirty : DiskState::Clean;
    }

    bool VFXEffectTypeDocument::SaveDocument()
    {
        if ( !m_Loaded || m_Assets == nullptr )
            return false;
        const Assets::AssetHandle handle( Subject().Owner );
        const auto*               meta = m_Assets->FindMetadataByHandle( handle );
        if ( meta == nullptr )
        {
            m_Refusal = "Save: the effect type asset is no longer held by the asset manager";
            return false;
        }
        if ( m_DiskConflict )
        {
            m_Refusal = "Save: the file changed on disk - choose Reload or Keep My Edits first";
            LOG_WARN( "[VFX] '{}' was not saved: the file changed on disk under unsaved edits.",
                      meta->Filepath.string() );
            return false;
        }
        if ( auto saved = Assets::VFXEffectTypeAsset::Save( meta->Filepath, m_Working ); !saved )
        {
            m_Refusal = std::format( "Save: {}", saved.GetError() );
            LOG_ERROR( "[VFX] '{}' was not saved: {}", meta->Filepath.string(), saved.GetError() );
            return false;
        }
        m_Saved = m_Working;
        m_Refusal.clear();
        // The held asset re-reads what was written, so every VFXWorld's next tick reads the new levels.
        if ( const auto asset = m_Assets->FindByHandle<Assets::VFXEffectTypeAsset>( handle ) )
        {
            if ( auto reloaded = asset->LoadFromFile(); !reloaded )
                LOG_ERROR( "[VFX] '{}' was saved but would not be read back: {}", meta->Filepath.string(),
                           reloaded.GetError() );
        }
        return true;
    }

    void VFXEffectTypeDocument::SyncWithDisk()
    {
        const Assets::AssetHandle handle( Subject().Owner );
        const auto                asset =
             m_Assets != nullptr ? m_Assets->FindByHandle<Assets::VFXEffectTypeAsset>( handle ) : nullptr;
        if ( !asset || !asset->IsReadyForUse() )
            return;
        const auto& disk = asset->GetData();
        if ( AuthoredOf( disk ) == AuthoredOf( m_Saved ) )
        {
            m_DiskConflict = false;
            return;
        }
        const std::string path = asset->GetMetadata().Filepath.generic_string();
        // Clean, or the edits already equal the file: the file is simply the new base.
        if ( AuthoredOf( m_Working ) == AuthoredOf( m_Saved ) || AuthoredOf( m_Working ) == AuthoredOf( disk ) )
        {
            m_Working      = disk;
            m_Saved        = disk;
            m_DiskConflict = false;
            LOG_INFO( "[VFX] Effect type document '{}' re-read the file changed on disk - levels: {}", path,
                      DescribeLevels( m_Saved.Levels ) );
            return;
        }
        if ( !m_DiskConflict )
            LOG_WARN(
                 "[VFX] Effect type document '{}': the file changed on disk ({}) while the window has unsaved "
                 "edits - Reload or Keep My Edits; Save is refused until then.",
                 path, DescribeLevels( disk.Levels ) );
        m_DiskConflict = true;
    }

    void VFXEffectTypeDocument::DrawDiskConflict()
    {
        if ( !m_DiskConflict )
            return;
        const auto asset =
             m_Assets->FindByHandle<Assets::VFXEffectTypeAsset>( Assets::AssetHandle( Subject().Owner ) );
        if ( !asset )
            return;
        ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.30f, 1.0f ),
                            ICON_MDI_ALERT " The file changed on disk while this window has unsaved edits." );
        if ( ImGui::Button( "Reload (discard my edits)" ) )
        {
            m_Working      = asset->GetData();
            m_Saved        = m_Working;
            m_DiskConflict = false;
            m_Refusal.clear();
            LOG_INFO( "[VFX] Effect type document reloaded from disk, unsaved edits discarded - levels: {}",
                      DescribeLevels( m_Saved.Levels ) );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Keep My Edits (Save overwrites the file)" ) )
        {
            m_Saved        = asset->GetData();
            m_DiskConflict = false;
            m_Refusal.clear();
        }
    }

    void VFXEffectTypeDocument::TrackDrag( const char* label )
    {
        if ( ImGui::IsItemActivated() )
            m_DragBefore = AuthoredOf( m_Working );
        if ( !ImGui::IsItemDeactivatedAfterEdit() )
            return;
        if ( auto valid = Assets::Serialization::ValidateVFXEffectTypeData( m_Working ); !valid )
        {
            m_Refusal        = std::format( "{}: {}", label, valid.GetError() );
            Assign( m_Working, m_DragBefore );
            return;
        }
        m_Refusal.clear();
        if ( AuthoredOf( m_Working ) == m_DragBefore )
            return;
        CommandHistory::Get().PushCommand( std::make_unique<VFXEffectTypeEditCommand>(
             &m_Working, std::move( m_DragBefore ), AuthoredOf( m_Working ), label ) );
        m_DragBefore = {};
    }

    void VFXEffectTypeDocument::CommitEdit( VFXEffectTypeAuthored before, const char* label )
    {
        if ( auto valid = Assets::Serialization::ValidateVFXEffectTypeData( m_Working ); !valid )
        {
            m_Refusal = std::format( "{}: {}", label, valid.GetError() );
            Assign( m_Working, before );
            return;
        }
        m_Refusal.clear();
        if ( AuthoredOf( m_Working ) == before )
            return;
        CommandHistory::Get().PushCommand( std::make_unique<VFXEffectTypeEditCommand>(
             &m_Working, std::move( before ), AuthoredOf( m_Working ), label ) );
    }

    void VFXEffectTypeDocument::OnUIRender()
    {
        if ( !m_Loaded )
        {
            ImGui::TextWrapped( "This effect type could not be read: %s", m_LoadError.c_str() );
            return;
        }

        SyncWithDisk();
        DrawDiskConflict();
        if ( ImGui::Button( ICON_MDI_CONTENT_SAVE " Save" ) )
            (void)SaveDocument();
        ImGui::SameLine();
        ImGui::TextDisabled( "Per Effects quality level (Effects.QualityLevel)" );
        if ( !m_Refusal.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", m_Refusal.c_str() );

        // The type: what a culled system does, and how a limit ranks its instances.
        {
            const Authored before   = AuthoredOf( m_Working );
            int            reaction = static_cast<int>( m_Working.CullReaction );
            ImGui::SetNextItemWidth( 240.0f );
            if ( ImGui::Combo( "Cull Reaction", &reaction, kReactionNames.data(),
                               static_cast<int>( kReactionNames.size() ) ) )
            {
                m_Working.CullReaction = static_cast<Assets::Serialization::VFXCullReaction>( reaction );
                CommitEdit( before, "Cull Reaction" );
            }
        }
        {
            const Authored before       = AuthoredOf( m_Working );
            int            significance = static_cast<int>( m_Working.Significance );
            ImGui::SetNextItemWidth( 240.0f );
            if ( ImGui::Combo( "Significance", &significance, kSignificanceNames.data(),
                               static_cast<int>( kSignificanceNames.size() ) ) )
            {
                m_Working.Significance = static_cast<Assets::Serialization::VFXSignificance>( significance );
                CommitEdit( before, "Significance" );
            }
        }

        constexpr ImGuiTableFlags kTable =
             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if ( !ImGui::BeginTable( "##levels", 7, kTable ) )
            return;
        ImGui::TableSetupColumn( "Level", ImGuiTableColumnFlags_WidthFixed, 90.0f );
        ImGui::TableSetupColumn( "Max Distance (cm, 0 = never culled)" );
        ImGui::TableSetupColumn( "Spawn Count Scale" );
        ImGui::TableSetupColumn( "Max Instances (type, 0 = no limit)" );
        ImGui::TableSetupColumn( "Max System Instances (0 = no limit)" );
        ImGui::TableSetupColumn( "Cull By View Frustum" );
        ImGui::TableSetupColumn( "Max Time Without Render (s, 0 = never)" );
        ImGui::TableHeadersRow();
        for ( std::size_t i = 0; i < m_Working.Levels.size() && i < kLevelNames.size(); ++i )
        {
            auto& level = m_Working.Levels[i];
            ImGui::PushID( static_cast<int>( i ) );
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( kLevelNames[i] );
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth( -1.0f );
            ImGui::DragFloat( "##distance", &level.MaxDistance, 10.0f, 0.0f, 1.0e7f, "%.0f cm",
                              ImGuiSliderFlags_AlwaysClamp );
            TrackDrag( "Max Distance" );
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth( -1.0f );
            ImGui::DragFloat( "##scale", &level.SpawnCountScale, 0.005f, 0.0f, 1.0f, "%.3f",
                              ImGuiSliderFlags_AlwaysClamp );
            TrackDrag( "Spawn Count Scale" );
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth( -1.0f );
            ImGui::DragInt( "##instances", &level.MaxInstances, 0.1f, 0, 100000, "%d",
                            ImGuiSliderFlags_AlwaysClamp );
            TrackDrag( "Max Instances" );
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth( -1.0f );
            ImGui::DragInt( "##systemInstances", &level.MaxSystemInstances, 0.1f, 0, 100000, "%d",
                            ImGuiSliderFlags_AlwaysClamp );
            TrackDrag( "Max System Instances" );
            ImGui::TableNextColumn();
            {
                const Authored before = AuthoredOf( m_Working );
                if ( ImGui::Checkbox( "##frustum", &level.CullByViewFrustum ) )
                    CommitEdit( before, "Cull By View Frustum" );
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth( -1.0f );
            ImGui::DragFloat( "##notRendered", &level.MaxTimeWithoutRender, 0.05f, 0.0f, 3600.0f, "%.2f s",
                              ImGuiSliderFlags_AlwaysClamp );
            TrackDrag( "Max Time Without Render" );
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestVFXEffectTypeDocument( Assets::AssetManager*        assets,
                                                                         const std::string&           path,
                                                                         const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr ||
             std::filesystem::path( path ).extension() != Assets::Serialization::kVFXEffectTypeExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::VFXEffectTypeAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::VFXEffectTypeAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[VFX] '{}' could not be registered as an effect type - no editor was opened.", path );
            return Outcome::Failed;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
            {
                LOG_ERROR( "[VFX] '{}' would not load as an effect type - no editor was opened: {}", path,
                           loaded.GetError() );
                return Outcome::Failed;
            }
        }

        const auto handle = asset->GetMetadata().Handle;
        if ( const auto opened = Core::RequestOpenAsset( assets->FindMetadataByHandle( handle ), handle, editors );
             !opened.IsSuccess() )
        {
            LOG_ERROR( "[VFX] '{}': {}", path, opened.GetError() );
            return Outcome::Failed;
        }
        return Outcome::Requested;
    }
} // namespace Desert::Editor
