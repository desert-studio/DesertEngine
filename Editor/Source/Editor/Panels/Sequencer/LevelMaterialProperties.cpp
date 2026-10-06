#include "LevelMaterialProperties.hpp"

#include <Editor/Core/Commands/SequenceEdit.hpp>

#include <Engine/ECS/LevelSequenceAuthoring.hpp>

#include <format>

namespace Desert::Editor::LevelMaterialEdit
{
    namespace LevelTL = Animation::Timeline;

    std::string SlotLabel( const uint32_t slot, const std::string& material )
    {
        return material.empty() ? std::format( "Slot {}", slot ) : std::format( "Slot {} ({})", slot, material );
    }

    std::string PropertyName( const std::string& actor, const ECS::LevelSequenceMaterialParameter& parameter )
    {
        return std::format( "{}.{}.{}", actor, parameter.Slot, parameter.Name );
    }

    const Schema* FindSchema( const std::vector<Schema>& schema, const LevelTL::BindingGuid& binding,
                              const ECS::LevelSequenceMaterialParameter& parameter )
    {
        for ( const auto& row : schema )
            if ( row.Binding == binding && row.Parameter == parameter )
                return &row;
        return nullptr;
    }

    namespace
    {
        int ComponentsOf( const LevelTL::TrackKind kind )
        {
            return kind == LevelTL::TrackKind::Float ? 1 : 3;
        }
    } // namespace

    std::vector<EditableProperty> Describe( const LevelTL::Sequence& sequence, const Animation::FrameNumber tick,
                                            const std::vector<Schema>& schema )
    {
        std::vector<EditableProperty> properties;
        for ( const auto& binding : sequence.Bindings )
        {
            if ( binding.Kind != LevelTL::BindingKind::Entity )
                continue;
            for ( const auto& [parameter, kind] : ECS::MaterialParameterTracks( sequence, binding.Guid ) )
            {
                const auto value = ECS::MaterialParameterAt( sequence, binding.Guid, parameter, tick );
                if ( !value )
                    continue;
                const Schema*    declared = FindSchema( schema, binding.Guid, parameter );
                EditableProperty property;
                property.Name       = PropertyName( binding.Label, parameter );
                property.Label      = declared != nullptr ? declared->Label : parameter.Name;
                property.Group      = std::format( "{} ▸ {}", binding.Label,
                                              declared != nullptr ? declared->SlotLabel
                                                                       : SlotLabel( parameter.Slot, std::string{} ) );
                property.Components = ComponentsOf( kind );
                if ( kind == LevelTL::TrackKind::Float )
                    property.Type = "float";
                else if ( declared != nullptr && declared->Color )
                    property.Type = "color";
                else
                    property.Type = "float3";
                if ( declared != nullptr )
                {
                    property.Min = declared->Min;
                    property.Max = declared->Max;
                }
                for ( int c = 0; c < property.Components; ++c )
                    property.Value[static_cast<size_t>( c )] = ( *value )[c];
                properties.push_back( std::move( property ) );
            }
        }
        return properties;
    }

    Common::ResultStr<Write> Resolve( const LevelTL::Sequence& sequence, const std::vector<Schema>& schema,
                                      const std::string& name, const std::vector<float>& value )
    {
        std::optional<Write> found;
        LevelTL::TrackKind   kind = LevelTL::TrackKind::Float;
        for ( const auto& binding : sequence.Bindings )
        {
            if ( binding.Kind != LevelTL::BindingKind::Entity )
                continue;
            for ( const auto& [parameter, trackKind] : ECS::MaterialParameterTracks( sequence, binding.Guid ) )
            {
                if ( PropertyName( binding.Label, parameter ) != name )
                    continue;
                if ( found )
                    return Common::MakeFormattedError<Write>(
                         "'{}' names a Material Parameter track of two actors with the same label; rename one "
                         "of them so the property names a single track.",
                         name );
                found = Write{ binding.Guid, parameter, glm::vec4( 0.0F ) };
                kind  = trackKind;
            }
        }
        if ( !found )
            return Common::MakeFormattedError<Write>(
                 "this Level Sequence has no Material Parameter track '{}' (<actor>.<slot>.<parameter>). Ask "
                 "'properties' for the tracks it has.",
                 name );

        const int wanted = ComponentsOf( kind );
        if ( static_cast<int>( value.size() ) != wanted )
            return Common::MakeFormattedError<Write>( "'{}' is a {} track and takes {} number(s); {} were sent.",
                                                      name, wanted == 1 ? "scalar" : "vector", wanted,
                                                      value.size() );

        // The schema's own clamp, refused rather than applied: the row's slider cannot leave it either.
        if ( const Schema* declared = FindSchema( schema, found->Binding, found->Parameter );
             declared != nullptr && declared->Min && declared->Max )
            for ( const float component : value )
                if ( component < *declared->Min || component > *declared->Max )
                    return Common::MakeFormattedError<Write>(
                         "'{}' is declared range({}, {}) and {} is outside it. The track row's own control "
                         "cannot leave that range either.",
                         name, *declared->Min, *declared->Max, component );

        for ( int c = 0; c < wanted; ++c )
            found->Value[c] = value[static_cast<size_t>( c )];
        return Common::MakeSuccess( std::move( *found ) );
    }

    Common::BoolResultStr Key( LevelTL::Sequence& sequence, SequenceEditTransaction& transaction,
                               const SequenceOwner& owner, const LevelTL::BindingGuid& binding,
                               const ECS::LevelSequenceMaterialParameter& parameter,
                               const Animation::FrameNumber tick, const glm::vec4& value )
    {
        const ScopedSequenceEdit undoStep( transaction, owner );
        return ECS::SetMaterialParameterKey( sequence, binding, parameter, tick, value );
    }
} // namespace Desert::Editor::LevelMaterialEdit
