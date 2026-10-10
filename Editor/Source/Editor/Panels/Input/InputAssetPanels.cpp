#include "InputAssetPanels.hpp"

#include <Editor/Core/EditorSubject.hpp>
#include <Editor/Core/SubjectTitle.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/EnhancedInputAssets.hpp>
#include <Engine/Input/InputKey.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

namespace Desert::Editor
{
    namespace S = Assets::Serialization;

    namespace
    {
        // The enumerators' spellings as the file writes them, in enumerator order.
        constexpr std::array<const char*, 4> kValueTypes    = { "Bool", "Axis1D", "Axis2D", "Axis3D" };
        constexpr std::array<const char*, 4> kModifierTypes = { "Negate", "Swizzle", "DeadZone", "Scalar" };
        constexpr std::array<const char*, 5> kSwizzleOrders = { "YXZ", "ZYX", "XZY", "YZX", "ZXY" };
        constexpr std::array<const char*, 2> kDeadZoneTypes = { "Axial", "Radial" };
        constexpr std::array<const char*, 4> kTriggerTypes  = { "Down", "Pressed", "Released", "Hold" };

        template <std::size_t N>
        bool EnumCombo( const char* label, int& value, const std::array<const char*, N>& names )
        {
            return ImGui::Combo( label, &value, names.data(), static_cast<int>( N ) );
        }

        // The subject as the asset holds it; a registered-but-unread asset is read now, so the window never
        // shows (and could never save) a default under the file's name.
        template <typename TAsset, typename Data>
        bool LoadSubject( Assets::AssetManager* assets, const Assets::AssetHandle& handle, Common::Filepath& path,
                          Data& data, std::string& status )
        {
            if ( assets == nullptr )
            {
                status = "no asset manager";
                return false;
            }
            const auto asset = assets->FindByHandle<TAsset>( handle );
            if ( !asset )
            {
                status = "the asset is not registered";
                return false;
            }
            if ( !asset->IsReadyForUse() )
                if ( const auto loaded = asset->LoadFromFile(); !loaded )
                {
                    status = std::string( loaded.GetError() );
                    return false;
                }
            path = asset->GetMetadata().Filepath;
            data = asset->GetData();
            return true;
        }

        // Writes through the asset serializer, then re-reads the asset so Play reads what was saved.
        template <typename TAsset, typename Data>
        Common::BoolResultStr SaveSubject( Assets::AssetManager* assets, const Assets::AssetHandle& handle,
                                           const Common::Filepath& path, const Data& data )
        {
            if ( path.empty() )
                return Common::MakeError( "the asset did not load; there is nothing to save over" );
            if ( auto saved = TAsset::Save( path, data ); !saved )
                return saved;
            if ( assets != nullptr )
                if ( const auto asset = assets->FindByHandle<TAsset>( handle ) )
                    if ( auto reread = asset->LoadFromFile(); !reread )
                        return reread;
            return Common::MakeSuccess( true );
        }

        void DrawStatus( const std::string& status, const bool isError )
        {
            if ( status.empty() )
                return;
            if ( isError )
                ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ), "%s", status.c_str() );
            else
                ImGui::TextDisabled( "%s", status.c_str() );
        }

        struct ActionChoice
        {
            std::string          Label;
            Assets::AssetGuidRef Ref;
        };

        // The project's Input Actions as the registry lists them (nothing is loaded to list them). The
        // reference's path is the registry's stable key (`root:relative/path`), for the reader only.
        std::vector<ActionChoice> ActionChoices()
        {
            std::vector<ActionChoice> choices;
            for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::InputAction ) )
            {
                if ( !row.Guid )
                    continue;
                ActionChoice choice;
                choice.Label    = row.DisplayName.empty() ? row.Path.stem().string() : row.DisplayName;
                choice.Ref.Guid = Common::Content::AssetGuidToText( *row.Guid );
                choice.Ref.Path = row.Key;
                choices.push_back( std::move( choice ) );
            }
            return choices;
        }
    } // namespace

    // ── Input Action ────────────────────────────────────────────────────────────────────────────────────

    InputActionPanel::InputActionPanel( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument( AssetSubjectTitle<Assets::InputActionAsset>( subject, assets, "Input Action" ),
                             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::InputAction ) ) ),
           m_Assets( assets ), m_Handle( subject ), m_Data( std::make_shared<S::InputActionData>() )
    {
        if ( LoadSubject<Assets::InputActionAsset>( m_Assets, m_Handle, m_Path, *m_Data, m_Status ) )
            m_OnDisk = *m_Data;
        else
            m_StatusIsError = true;
    }

    InputAssetOwner<S::InputActionData> InputActionPanel::Owner() const
    {
        InputAssetOwner<S::InputActionData> owner;
        owner.Label   = "Input Action '" + m_Path.stem().string() + "'";
        owner.Resolve = [weak = std::weak_ptr<S::InputActionData>( m_Data )]() -> S::InputActionData*
        {
            const auto data = weak.lock();
            return data.get(); // the window owns the copy: a closed window resolves to nothing
        };
        return owner;
    }

    void InputActionPanel::OnUIRender()
    {
        // No ImGui::Begin: the editor's panel loop wraps OnUIRender in this panel's window.
        if ( m_Path.empty() )
        {
            DrawStatus( m_Status, true );
            return;
        }
        S::InputActionData& data = *m_Data;

        int valueType = static_cast<int>( data.ValueType );
        if ( EnumCombo( "Value Type", valueType, kValueTypes ) )
            (void)m_Edits.Edit(
                 Owner(), [valueType]( S::InputActionData& d )
                 { return InputEdit::SetValueType( d, static_cast<S::InputValueType>( valueType ) ); } );
        ImGui::Checkbox( "Consume Input", &data.ConsumeInput );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip(
                 "A key this action reads is not offered to lower-priority contexts (UE bConsumeInput)" );

        ImGui::Separator();
        if ( ImGui::Button( "Save" ) )
            (void)SaveDocument();
        DrawStatus( m_Status, m_StatusIsError );

        (void)m_Edits.Observe( Owner(), ImGui::IsAnyItemActive() );
    }

    bool InputActionPanel::IsSubjectAlive() const
    {
        return m_Assets && m_Assets->FindMetadataByHandle( m_Handle ) != nullptr;
    }

    ISubjectDocument::DiskState InputActionPanel::GetDiskState() const
    {
        if ( m_Path.empty() )
            return DiskState::Untracked;
        return *m_Data == m_OnDisk ? DiskState::Clean : DiskState::Dirty;
    }

    bool InputActionPanel::SaveDocument()
    {
        const auto saved = SaveSubject<Assets::InputActionAsset>( m_Assets, m_Handle, m_Path, *m_Data );
        m_StatusIsError  = !saved;
        m_Status         = saved ? "Saved " + m_Path.filename().string() : std::string( saved.GetError() );
        if ( !saved )
        {
            LOG_ERROR( "[Input] '{}' was not saved: {}", m_Path.string(), saved.GetError() );
            return false;
        }
        m_OnDisk = *m_Data;
        return true;
    }

    // ── Input Mapping Context ───────────────────────────────────────────────────────────────────────────

    InputMappingContextPanel::InputMappingContextPanel( const Assets::AssetHandle& subject,
                                                        Assets::AssetManager*      assets )
         : ISubjectDocument(
                AssetSubjectTitle<Assets::InputMappingContextAsset>( subject, assets, "Input Mapping Context" ),
                AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::InputMappingContext ) ) ),
           m_Assets( assets ), m_Handle( subject ), m_Data( std::make_shared<S::InputMappingContextData>() )
    {
        if ( LoadSubject<Assets::InputMappingContextAsset>( m_Assets, m_Handle, m_Path, *m_Data, m_Status ) )
            m_OnDisk = *m_Data;
        else
            m_StatusIsError = true;
    }

    InputAssetOwner<S::InputMappingContextData> InputMappingContextPanel::Owner() const
    {
        InputAssetOwner<S::InputMappingContextData> owner;
        owner.Label   = "Input Mapping Context '" + m_Path.stem().string() + "'";
        owner.Resolve = [weak =
                              std::weak_ptr<S::InputMappingContextData>( m_Data )]() -> S::InputMappingContextData*
        {
            const auto data = weak.lock();
            return data.get();
        };
        return owner;
    }

    void InputMappingContextPanel::OnUIRender()
    {
        if ( m_Path.empty() )
        {
            DrawStatus( m_Status, true );
            return;
        }
        using Operation = InputContextEditTransaction::Operation;

        S::InputMappingContextData&           data    = *m_Data;
        static const std::vector<std::string> keys    = Input::InputKeyNames();
        const std::vector<ActionChoice>       actions = ActionChoices();
        // At most one structural edit per frame, applied after the loop: an erase inside it would move the
        // elements the loop is still drawing.
        Operation pending;

        for ( std::size_t i = 0; i < data.Mappings.size(); ++i )
        {
            S::InputKeyMappingData& mapping = data.Mappings[i];
            ImGui::PushID( static_cast<int>( i ) );

            std::string current = mapping.Action.Path + " (not in this project)";
            for ( const ActionChoice& choice : actions )
                if ( choice.Ref.Guid == mapping.Action.Guid )
                    current = choice.Label;
            ImGui::SetNextItemWidth( 200.0f );
            if ( ImGui::BeginCombo( "##action", current.c_str() ) )
            {
                for ( const ActionChoice& choice : actions )
                    if ( ImGui::Selectable( choice.Label.c_str(), choice.Ref.Guid == mapping.Action.Guid ) )
                        pending = [i, ref = choice.Ref]( S::InputMappingContextData& d )
                        { return InputEdit::SetMappingAction( d, i, ref ); };
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 140.0f );
            if ( ImGui::BeginCombo( "##key", mapping.Key.c_str() ) )
            {
                for ( const std::string& key : keys )
                    if ( ImGui::Selectable( key.c_str(), key == mapping.Key ) )
                        pending = [i, key]( S::InputMappingContextData& d )
                        { return InputEdit::SetMappingKey( d, i, key ); };
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Remove" ) )
                pending = [i]( S::InputMappingContextData& d ) { return InputEdit::RemoveMapping( d, i ); };

            if ( ImGui::TreeNode( "Modifiers", "Modifiers (%zu, applied in order)", mapping.Modifiers.size() ) )
            {
                for ( std::size_t m = 0; m < mapping.Modifiers.size(); ++m )
                {
                    S::InputModifierData& modifier = mapping.Modifiers[m];
                    ImGui::PushID( static_cast<int>( m ) );
                    ImGui::Text( "%zu. %s", m + 1, kModifierTypes[static_cast<std::size_t>( modifier.Type )] );
                    ImGui::SameLine();
                    if ( m > 0 && ImGui::SmallButton( "Up" ) )
                        pending = [i, m]( S::InputMappingContextData& d )
                        { return InputEdit::MoveModifier( d, i, m, m - 1 ); };
                    ImGui::SameLine();
                    if ( m + 1 < mapping.Modifiers.size() && ImGui::SmallButton( "Down" ) )
                        pending = [i, m]( S::InputMappingContextData& d )
                        { return InputEdit::MoveModifier( d, i, m, m + 1 ); };
                    ImGui::SameLine();
                    if ( ImGui::SmallButton( "Remove" ) )
                        pending = [i, m]( S::InputMappingContextData& d )
                        { return InputEdit::RemoveModifier( d, i, m ); };
                    ImGui::Indent();
                    if ( modifier.Negate )
                    {
                        ImGui::Checkbox( "X", &modifier.Negate->X );
                        ImGui::SameLine();
                        ImGui::Checkbox( "Y", &modifier.Negate->Y );
                        ImGui::SameLine();
                        ImGui::Checkbox( "Z", &modifier.Negate->Z );
                    }
                    if ( modifier.Swizzle )
                    {
                        int order = static_cast<int>( modifier.Swizzle->Order );
                        if ( EnumCombo( "Order", order, kSwizzleOrders ) )
                            modifier.Swizzle->Order = static_cast<S::InputSwizzleOrder>( order );
                    }
                    if ( modifier.DeadZone )
                    {
                        int type = static_cast<int>( modifier.DeadZone->Type );
                        if ( EnumCombo( "Type", type, kDeadZoneTypes ) )
                            modifier.DeadZone->Type = static_cast<S::InputDeadZoneType>( type );
                        ImGui::DragFloat( "Lower Threshold", &modifier.DeadZone->LowerThreshold, 0.01f, 0.0f,
                                          1.0f );
                        ImGui::DragFloat( "Upper Threshold", &modifier.DeadZone->UpperThreshold, 0.01f, 0.0f,
                                          1.0f );
                    }
                    if ( modifier.Scalar )
                        ImGui::DragFloat3( "Scalar", &modifier.Scalar->Scalar.x, 0.01f );
                    ImGui::Unindent();
                    ImGui::PopID();
                }
                int added = -1;
                ImGui::SetNextItemWidth( 160.0f );
                if ( EnumCombo( "Add Modifier", added, kModifierTypes ) && added >= 0 )
                    pending = [i, added]( S::InputMappingContextData& d )
                    { return InputEdit::AddModifier( d, i, static_cast<S::InputModifierType>( added ) ); };
                ImGui::TreePop();
            }

            if ( ImGui::TreeNode( "Triggers", "Triggers (%zu)", mapping.Triggers.size() ) )
            {
                if ( mapping.Triggers.empty() )
                    ImGui::TextDisabled( "None: triggered every frame the modified value is not zero" );
                for ( std::size_t t = 0; t < mapping.Triggers.size(); ++t )
                {
                    S::InputTriggerData& trigger = mapping.Triggers[t];
                    ImGui::PushID( static_cast<int>( t ) );
                    int type = static_cast<int>( trigger.Type );
                    ImGui::SetNextItemWidth( 120.0f );
                    if ( EnumCombo( "##type", type, kTriggerTypes ) )
                        pending = [i, t, type]( S::InputMappingContextData& d )
                        { return InputEdit::SetTriggerType( d, i, t, static_cast<S::InputTriggerType>( type ) ); };
                    ImGui::SameLine();
                    if ( ImGui::SmallButton( "Remove" ) )
                        pending = [i, t]( S::InputMappingContextData& d )
                        { return InputEdit::RemoveTrigger( d, i, t ); };
                    ImGui::Indent();
                    ImGui::DragFloat( "Actuation Threshold", &trigger.ActuationThreshold, 0.01f, 0.0f, 1.0f );
                    if ( trigger.Hold )
                    {
                        ImGui::DragFloat( "Hold Seconds", &trigger.Hold->HoldTimeSeconds, 0.01f, 0.01f, 60.0f,
                                          "%.2f s" );
                        ImGui::Checkbox( "One Shot", &trigger.Hold->IsOneShot );
                    }
                    ImGui::Unindent();
                    ImGui::PopID();
                }
                int added = -1;
                ImGui::SetNextItemWidth( 160.0f );
                if ( EnumCombo( "Add Trigger", added, kTriggerTypes ) && added >= 0 )
                    pending = [i, added]( S::InputMappingContextData& d )
                    { return InputEdit::AddTrigger( d, i, static_cast<S::InputTriggerType>( added ) ); };
                ImGui::TreePop();
            }
            ImGui::Separator();
            ImGui::PopID();
        }

        if ( ImGui::Button( "Add Mapping" ) )
        {
            if ( actions.empty() )
            {
                m_Status        = "Create an Input Action first: a mapping maps a key to an action";
                m_StatusIsError = true;
            }
            else
                pending = [ref = actions.front().Ref, key = keys.front()]( S::InputMappingContextData& d )
                { return InputEdit::AddMapping( d, ref, key ); };
        }

        if ( pending )
            if ( const auto edited = m_Edits.Edit( Owner(), pending ); !edited )
            {
                m_Status        = std::string( edited.GetError() );
                m_StatusIsError = true;
            }

        ImGui::Separator();
        if ( ImGui::Button( "Save" ) )
            (void)SaveDocument();
        DrawStatus( m_Status, m_StatusIsError );

        (void)m_Edits.Observe( Owner(), ImGui::IsAnyItemActive() );
    }

    bool InputMappingContextPanel::IsSubjectAlive() const
    {
        return m_Assets && m_Assets->FindMetadataByHandle( m_Handle ) != nullptr;
    }

    ISubjectDocument::DiskState InputMappingContextPanel::GetDiskState() const
    {
        if ( m_Path.empty() )
            return DiskState::Untracked;
        return *m_Data == m_OnDisk ? DiskState::Clean : DiskState::Dirty;
    }

    bool InputMappingContextPanel::SaveDocument()
    {
        // A context the subsystem could not evaluate is refused with the mapping named, never written.
        const Common::BoolResultStr saved = [this]() -> Common::BoolResultStr
        {
            if ( auto valid = S::ValidateInputMappingContext( *m_Data ); !valid )
                return valid;
            return SaveSubject<Assets::InputMappingContextAsset>( m_Assets, m_Handle, m_Path, *m_Data );
        }();
        m_StatusIsError = !saved;
        m_Status        = saved ? "Saved " + m_Path.filename().string() : std::string( saved.GetError() );
        if ( !saved )
        {
            LOG_ERROR( "[Input] '{}' was not saved: {}", m_Path.string(), saved.GetError() );
            return false;
        }
        m_OnDisk = *m_Data;
        return true;
    }
} // namespace Desert::Editor
