#include <Engine/Assets/Serialization/InputAssets.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Engine/Input/InputKey.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace Desert::Assets::Serialization
{
    namespace
    {
        // The action GUIDs a context's header states: first-mention order, each once.
        std::vector<std::string> DependenciesOf( const InputMappingContextData& data )
        {
            std::vector<std::string> out;
            for ( const InputKeyMappingData& mapping : data.Mappings )
                if ( std::find( out.begin(), out.end(), mapping.Action.Guid ) == out.end() )
                    out.push_back( mapping.Action.Guid );
            return out;
        }

        // How many parameter blocks a modifier states; exactly the one its Type reads is allowed.
        Common::BoolResultStr CheckModifier( const InputModifierData& m, const std::string& where )
        {
            const int stated =
                 ( m.Negate ? 1 : 0 ) + ( m.Swizzle ? 1 : 0 ) + ( m.DeadZone ? 1 : 0 ) + ( m.Scalar ? 1 : 0 );
            bool own = false;
            switch ( m.Type )
            {
                case InputModifierType::Negate:
                    own = m.Negate.has_value();
                    break;
                case InputModifierType::Swizzle:
                    own = m.Swizzle.has_value();
                    break;
                case InputModifierType::DeadZone:
                    own = m.DeadZone.has_value();
                    break;
                case InputModifierType::Scalar:
                    own = m.Scalar.has_value();
                    break;
            }
            if ( !own || stated != 1 )
                return Common::MakeFormattedError<bool>(
                     "{}: a modifier states exactly the one parameter block its Type reads ({} stated)", where,
                     stated );
            if ( m.DeadZone )
            {
                const InputDeadZoneParams& dz = *m.DeadZone;
                if ( !std::isfinite( dz.LowerThreshold ) || !std::isfinite( dz.UpperThreshold ) ||
                     dz.LowerThreshold < 0.0f || dz.UpperThreshold <= dz.LowerThreshold )
                    return Common::MakeFormattedError<bool>(
                         "{}: DeadZone needs 0 <= LowerThreshold < UpperThreshold (got {} / {})", where,
                         dz.LowerThreshold, dz.UpperThreshold );
            }
            if ( m.Scalar && !( std::isfinite( m.Scalar->Scalar.x ) && std::isfinite( m.Scalar->Scalar.y ) &&
                                std::isfinite( m.Scalar->Scalar.z ) ) )
                return Common::MakeFormattedError<bool>( "{}: Scalar is not finite", where );
            return BOOLSUCCESS;
        }

        Common::BoolResultStr CheckTrigger( const InputTriggerData& t, const std::string& where )
        {
            if ( ( t.Type == InputTriggerType::Hold ) != t.Hold.has_value() )
                return Common::MakeFormattedError<bool>(
                     "{}: the Hold block is stated by a Hold trigger and by no other", where );
            if ( !std::isfinite( t.ActuationThreshold ) || t.ActuationThreshold <= 0.0f )
                return Common::MakeFormattedError<bool>( "{}: ActuationThreshold must be > 0 (got {})", where,
                                                         t.ActuationThreshold );
            if ( t.Hold && !( std::isfinite( t.Hold->HoldTimeSeconds ) && t.Hold->HoldTimeSeconds > 0.0f ) )
                return Common::MakeFormattedError<bool>( "{}: HoldTimeSeconds must be > 0 (got {})", where,
                                                         t.Hold->HoldTimeSeconds );
            return BOOLSUCCESS;
        }

        Common::BoolResultStr CreateParent( const std::filesystem::path& path )
        {
            std::error_code ec;
            if ( path.has_parent_path() )
                std::filesystem::create_directories( path.parent_path(), ec );
            if ( ec )
                return Common::MakeFormattedError<bool>( "cannot create '{}': {}", path.parent_path().string(),
                                                         ec.message() );
            return BOOLSUCCESS;
        }
    } // namespace

    Common::BoolResultStr ValidateInputMappingContext( const InputMappingContextData& data )
    {
        for ( std::size_t i = 0; i < data.Mappings.size(); ++i )
        {
            const InputKeyMappingData& mapping = data.Mappings[i];
            const std::string          where   = std::format( "mapping {} (key '{}')", i, mapping.Key );
            if ( !Desert::Input::InputKeyFromName( mapping.Key ) )
                return Common::MakeFormattedError<bool>( "{}: '{}' names no key", where, mapping.Key );
            if ( const auto guid = Common::Content::AssetGuidFromText( mapping.Action.Guid );
                 !guid || guid.GetValue().IsNull() )
                return Common::MakeFormattedError<bool>( "{}: Action.Guid '{}' is not an asset GUID", where,
                                                         mapping.Action.Guid );
            for ( std::size_t m = 0; m < mapping.Modifiers.size(); ++m )
                if ( auto ok = CheckModifier( mapping.Modifiers[m], std::format( "{} modifier {}", where, m ) );
                     !ok )
                    return ok;
            for ( std::size_t t = 0; t < mapping.Triggers.size(); ++t )
                if ( auto ok = CheckTrigger( mapping.Triggers[t], std::format( "{} trigger {}", where, t ) ); !ok )
                    return ok;
        }
        return BOOLSUCCESS;
    }

    Common::ResultStr<InputActionData> ParseInputAction( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<InputActionData>( "the file is empty" );
        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kInputActionVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<InputActionData>( "input action {}", headed.GetError() );
        const auto parsed = Common::Json::Read<InputActionData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<InputActionData>( "{}", parsed.GetError() );
        InputActionData data = parsed.GetValue();
        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::InputAction,
                                                      Assets::kInputActionSchemaTag, kInputActionVersion,
                                                      InputActionTextSubsystems() );
             !header )
            return Common::MakeFormattedError<InputActionData>( "input action {}", header.GetError() );
        if ( !data.Header->Dependencies.empty() )
            return Common::MakeFormattedError<InputActionData>(
                 "the header states {} Dependencies; an input action references no asset",
                 data.Header->Dependencies.size() );
        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<InputMappingContextData> ParseInputMappingContext( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<InputMappingContextData>( "the file is empty" );
        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kInputMappingContextVersion, std::nullopt );
             !headed )
            return Common::MakeFormattedError<InputMappingContextData>( "input mapping context {}",
                                                                        headed.GetError() );
        const auto parsed = Common::Json::Read<InputMappingContextData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<InputMappingContextData>( "{}", parsed.GetError() );
        InputMappingContextData data = parsed.GetValue();
        if ( auto header =
                  Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::InputMappingContext,
                                             Assets::kInputMappingContextSchemaTag, kInputMappingContextVersion,
                                             InputMappingContextTextSubsystems() );
             !header )
            return Common::MakeFormattedError<InputMappingContextData>( "input mapping context {}",
                                                                        header.GetError() );
        if ( auto valid = ValidateInputMappingContext( data ); !valid )
            return Common::MakeFormattedError<InputMappingContextData>( "{}", valid.GetError() );
        // The header's edges are the actions the mappings name; a file where the two disagree would have the
        // registry and the subsystem see different actions.
        if ( data.Header->Dependencies != DependenciesOf( data ) )
            return Common::MakeFormattedError<InputMappingContextData>(
                 "the header's Dependencies ({} entries) are not exactly the mapped actions' GUIDs",
                 data.Header->Dependencies.size() );
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteInputAction( const InputActionData& data )
    {
        InputActionData out = data;
        out.Header          = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::InputAction,
                                                       InputActionTextSubsystems() );
        return Common::Json::Write( out );
    }

    std::string WriteInputMappingContext( const InputMappingContextData& data )
    {
        InputMappingContextData out = data;
        out.Header = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::InputMappingContext,
                                              InputMappingContextTextSubsystems() );
        out.Header->Dependencies = DependenciesOf( data );
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveInputActionFile( const std::filesystem::path& path, const InputActionData& data )
    {
        if ( auto made = CreateParent( path ); !made )
            return Common::MakeFormattedError<bool>( "input action '{}': {}", path.string(), made.GetError() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteInputAction( data ) );
    }

    Common::BoolResultStr SaveInputMappingContextFile( const std::filesystem::path&   path,
                                                       const InputMappingContextData& data )
    {
        if ( auto valid = ValidateInputMappingContext( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write input mapping context '{}': {}",
                                                     path.string(), valid.GetError() );
        if ( auto made = CreateParent( path ); !made )
            return Common::MakeFormattedError<bool>( "input mapping context '{}': {}", path.string(),
                                                     made.GetError() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteInputMappingContext( data ) );
    }
} // namespace Desert::Assets::Serialization
