#pragma once

// ENHANCED INPUT ASSETS (GP1a) — UE's UInputAction and UInputMappingContext as text assets with a header GUID,
// adapted: plain structs read by reflect-cpp instead of UObjects. An ACTION states only what it produces (its
// value type) and whether it consumes the keys mapped to it; a MAPPING CONTEXT maps keys to actions (named
// by {Guid, Path}, the header's Dependencies), each mapping with its modifier stack and its triggers.
//
// A modifier and a trigger are a Type plus the ONE parameter block that type reads, every other block absent:
// a file that states a block its type does not read is refused, so no field is a dead setting.

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/vec3.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    inline constexpr const char* kInputActionExtension         = ".deinputaction";
    inline constexpr const char* kInputMappingContextExtension = ".deinputcontext";

    /// 1 - GP1a: ValueType, ConsumeInput.
    inline constexpr int32_t kInputActionVersion = static_cast<int32_t>( Assets::kInputActionSchemaVersion );
    /// 1 - GP1a: Mappings (Action {Guid, Path}, Key by name, Modifiers, Triggers).
    inline constexpr int32_t kInputMappingContextVersion =
         static_cast<int32_t>( Assets::kInputMappingContextSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> InputActionTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kInputActionSchemaTag,
                                                static_cast<uint32_t>( kInputActionVersion ) } };
        return versions;
    }

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> InputMappingContextTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kInputMappingContextSchemaTag,
                                                static_cast<uint32_t>( kInputMappingContextVersion ) } };
        return versions;
    }

    /// UE's EInputActionValueType: how many components the action's value has (a Bool is 0 or 1 in X).
    enum class InputValueType
    {
        Bool,
        Axis1D,
        Axis2D,
        Axis3D,
    };

    /// UE's UInputAction. ConsumeInput (UE bConsumeInput): a key mapped to this action in a context is not
    /// seen by any lower-priority context.
    struct InputActionData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        InputValueType ValueType    = InputValueType::Bool;
        bool           ConsumeInput = true;

        [[nodiscard]] bool operator==( const InputActionData& ) const = default;
    };

    // ---- Modifiers (UE UInputModifier*), applied in the mapping's order to the key's value ----

    enum class InputModifierType
    {
        Negate,   // UInputModifierNegate
        Swizzle,  // UInputModifierSwizzleAxis
        DeadZone, // UInputModifierDeadZone
        Scalar,   // UInputModifierScalar
    };

    struct InputNegateParams
    {
        bool X = true;
        bool Y = true;
        bool Z = true;

        [[nodiscard]] bool operator==( const InputNegateParams& ) const = default;
    };

    /// UE's EInputAxisSwizzle: the order the output takes the input's components in.
    enum class InputSwizzleOrder
    {
        YXZ,
        ZYX,
        XZY,
        YZX,
        ZXY,
    };

    struct InputSwizzleParams
    {
        InputSwizzleOrder Order = InputSwizzleOrder::YXZ;

        [[nodiscard]] bool operator==( const InputSwizzleParams& ) const = default;
    };

    /// UE's EDeadZoneType: Axial treats each component alone, Radial the vector's length.
    enum class InputDeadZoneType
    {
        Axial,
        Radial,
    };

    /// Below LowerThreshold the value is 0; between the two it is remapped onto 0..1; above Upper it is 1.
    struct InputDeadZoneParams
    {
        InputDeadZoneType Type           = InputDeadZoneType::Radial;
        float             LowerThreshold = 0.2f;
        float             UpperThreshold = 1.0f;

        [[nodiscard]] bool operator==( const InputDeadZoneParams& ) const = default;
    };

    struct InputScalarParams
    {
        glm::vec3 Scalar{ 1.0f };

        [[nodiscard]] bool operator==( const InputScalarParams& ) const = default;
    };

    struct InputModifierData
    {
        InputModifierType                  Type = InputModifierType::Negate;
        std::optional<InputNegateParams>   Negate;
        std::optional<InputSwizzleParams>  Swizzle;
        std::optional<InputDeadZoneParams> DeadZone;
        std::optional<InputScalarParams>   Scalar;

        [[nodiscard]] bool operator==( const InputModifierData& ) const = default;
    };

    // ---- Triggers (UE UInputTrigger*), all explicit: the mapping is triggered when any of them is ----

    enum class InputTriggerType
    {
        Down,     // UInputTriggerDown: triggered every frame the value is actuated
        Pressed,  // UInputTriggerPressed: triggered on the one frame actuation begins
        Released, // UInputTriggerReleased: ongoing while actuated, triggered on the frame it ends
        Hold,     // UInputTriggerHold: ongoing while actuated, triggered once held HoldTimeSeconds
    };

    struct InputHoldParams
    {
        float HoldTimeSeconds = 1.0f;
        bool  IsOneShot       = false; ///< UE bIsOneShot: triggered on one frame only, then ongoing

        [[nodiscard]] bool operator==( const InputHoldParams& ) const = default;
    };

    struct InputTriggerData
    {
        InputTriggerType Type               = InputTriggerType::Down;
        float            ActuationThreshold = 0.5f; ///< UE: the value's length that counts as actuated
        std::optional<InputHoldParams> Hold;

        [[nodiscard]] bool operator==( const InputTriggerData& ) const = default;
    };

    /// UE's FEnhancedActionKeyMapping. No triggers: triggered while the modified value is non-zero (UE).
    struct InputKeyMappingData
    {
        AssetGuidRef                   Action;
        std::string                    Key; ///< InputKeyFromName's spelling
        std::vector<InputModifierData> Modifiers;
        std::vector<InputTriggerData>  Triggers;

        [[nodiscard]] bool operator==( const InputKeyMappingData& ) const = default;
    };

    /// UE's UInputMappingContext.
    struct InputMappingContextData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::vector<InputKeyMappingData> Mappings;

        [[nodiscard]] bool operator==( const InputMappingContextData& ) const = default;
    };

    /// Refuses what the subsystem could not evaluate, naming the mapping and the modifier or trigger.
    Common::BoolResultStr ValidateInputMappingContext( const InputMappingContextData& data );

    /// A file without a header, of another version or kind, or (a context) failing validation or stating
    /// Dependencies other than its actions' GUIDs is an error naming why.
    Common::ResultStr<InputActionData>         ParseInputAction( const std::string& text );
    Common::ResultStr<InputMappingContextData> ParseInputMappingContext( const std::string& text );

    /// Canonical text; stamps the header (keeping a loaded GUID, minting one otherwise). A context's
    /// Dependencies are its mappings' action GUIDs, first-mention order, each once.
    std::string WriteInputAction( const InputActionData& data );
    std::string WriteInputMappingContext( const InputMappingContextData& data );

    Common::BoolResultStr SaveInputActionFile( const std::filesystem::path& path, const InputActionData& data );
    Common::BoolResultStr SaveInputMappingContextFile( const std::filesystem::path&   path,
                                                       const InputMappingContextData& data );
} // namespace Desert::Assets::Serialization
