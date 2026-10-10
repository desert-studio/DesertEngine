// GP2: the engine's hand-written Enhanced Input content (Editor/Resources/Engine/Input) parses through the asset
// serializer, and IMC_Default binds the playable character's keys to the right actions with the right modifiers
// (UE third-person template: W/S on Move's Y through a YXZ swizzle, S and A negated, mouse look with Y negated).
#include <Engine/Assets/Serialization/InputAssets.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Assets::Serialization;

namespace
{
    std::string InputContentDir()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 8; ++up )
        {
            if ( std::filesystem::exists( prefix + "Editor/Resources/Engine/Input/IMC_Default.deinputcontext" ) )
                return prefix + "Editor/Resources/Engine/Input/";
            prefix += "../";
        }
        return {};
    }

    std::string ReadText( const std::string& path )
    {
        const std::ifstream in( path );
        std::ostringstream  ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Each action's file and the value type its consumer (PlayerController.lua) reads.
    struct ActionFile
    {
        const char*    File;
        InputValueType ValueType;
    };
    constexpr ActionFile kActions[] = {
         { "IA_Move.deinputaction", InputValueType::Axis2D },
         { "IA_Look.deinputaction", InputValueType::Axis2D },
         { "IA_Jump.deinputaction", InputValueType::Bool },
         { "IA_Crouch.deinputaction", InputValueType::Bool },
    };

    bool HasModifier( const InputKeyMappingData& mapping, InputModifierType type )
    {
        for ( const auto& m : mapping.Modifiers )
            if ( m.Type == type )
                return true;
        return false;
    }
} // namespace

TEST( EngineInputContent, EveryEngineInputActionParsesWithItsValueType )
{
    const std::string dir = InputContentDir();
    ASSERT_FALSE( dir.empty() ) << "run from inside the repository";
    for ( const auto& action : kActions )
    {
        const auto parsed = ParseInputAction( ReadText( dir + action.File ) );
        ASSERT_TRUE( parsed ) << action.File << ": " << parsed.GetError();
        EXPECT_EQ( parsed.GetValue().ValueType, action.ValueType ) << action.File;
        ASSERT_TRUE( parsed.GetValue().Header ) << action.File;
        EXPECT_EQ( parsed.GetValue().Header->Kind, "InputAction" ) << action.File;
    }
}

TEST( EngineInputContent, DefaultContextMapsWasdSpaceCtrlAndMouseToTheirActions )
{
    const std::string dir = InputContentDir();
    ASSERT_FALSE( dir.empty() ) << "run from inside the repository";

    // Action file -> its GUID, read from the action's own header (the context must point at these).
    std::map<std::string, std::string> guidOf;
    for ( const auto& action : kActions )
    {
        const auto parsed = ParseInputAction( ReadText( dir + action.File ) );
        ASSERT_TRUE( parsed ) << action.File << ": " << parsed.GetError();
        guidOf[action.File] = parsed.GetValue().Header->Guid;
    }

    const auto parsed = ParseInputMappingContext( ReadText( dir + "IMC_Default.deinputcontext" ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    const InputMappingContextData& context = parsed.GetValue();
    const auto                     valid   = ValidateInputMappingContext( context );
    EXPECT_TRUE( valid ) << ( valid ? "" : valid.GetError() );

    std::map<std::string, const InputKeyMappingData*> byKey;
    for ( const auto& mapping : context.Mappings )
    {
        EXPECT_EQ( byKey.count( mapping.Key ), 0u ) << mapping.Key << " is mapped twice";
        byKey[mapping.Key] = &mapping;
    }

    struct Expected
    {
        const char* Key;
        const char* ActionFile;
        bool        Swizzle; // YXZ: the key drives the action's Y (forward)
        bool        Negate;
    };
    const Expected expected[] = {
         { "W", "IA_Move.deinputaction", true, false },
         { "S", "IA_Move.deinputaction", true, true },
         { "D", "IA_Move.deinputaction", false, false },
         { "A", "IA_Move.deinputaction", false, true },
         { "Mouse2D", "IA_Look.deinputaction", false, true },
         { "Space", "IA_Jump.deinputaction", false, false },
         { "LeftControl", "IA_Crouch.deinputaction", false, false },
         { "C", "IA_Crouch.deinputaction", false, false },
    };
    EXPECT_EQ( context.Mappings.size(), std::size( expected ) ) << "IMC_Default carries a mapping no row states";

    for ( const auto& e : expected )
    {
        const auto found = byKey.find( e.Key );
        ASSERT_NE( found, byKey.end() ) << e.Key << " is not mapped in IMC_Default";
        const InputKeyMappingData& mapping = *found->second;
        EXPECT_EQ( mapping.Action.Guid, guidOf[e.ActionFile] ) << e.Key << " is not mapped to " << e.ActionFile;
        EXPECT_EQ( HasModifier( mapping, InputModifierType::Swizzle ), e.Swizzle ) << e.Key << " swizzle";
        EXPECT_EQ( HasModifier( mapping, InputModifierType::Negate ), e.Negate ) << e.Key << " negate";
        EXPECT_TRUE( mapping.Triggers.empty() ) << e.Key << ": the default context fires on Down (no triggers)";
        for ( const auto& m : mapping.Modifiers )
        {
            if ( m.Type == InputModifierType::Swizzle )
            {
                ASSERT_TRUE( m.Swizzle ) << e.Key;
                EXPECT_EQ( m.Swizzle->Order, InputSwizzleOrder::YXZ ) << e.Key;
            }
            if ( m.Type == InputModifierType::Negate )
            {
                ASSERT_TRUE( m.Negate ) << e.Key;
                const bool mouse = std::string( e.Key ) == "Mouse2D";
                // Movement keys negate the whole value; the mouse negates Y only (screen-down is pitch-up).
                EXPECT_EQ( m.Negate->X, !mouse ) << e.Key;
                EXPECT_TRUE( m.Negate->Y ) << e.Key;
                EXPECT_EQ( m.Negate->Z, !mouse ) << e.Key;
            }
        }
    }
}
