#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Reflection/Value.hpp>

namespace Desert::Reflection
{
    /// A REFLECTED FIELD READ AND WRITTEN AS A Value — the field half of the layer FunctionInfo::Invoke is the
    /// call half of. A language binding, a console and a test read and write a field through these two, so
    /// "which C++ type sits at this offset" is decided once, here, from FieldInfo::Type and FieldInfo::Size,
    /// and never again by a binding.
    ///
    /// The integer width is the field's own (Size 1/2/4/8), not an assumption of int32: an int64_t field
    /// reads all of its bits and a write that does not fit the field's width is refused, not wrapped.
    ///
    /// An AssetHandle field reads and writes as its 64-bit id, a UInt Value. A Struct field is reached through
    /// its StructType's fields at Offset, a container through FieldInfo::ContainerGet/Set (ContainerAccess.hpp):
    /// neither has a Value form, and both are refused here with that reason.

    /// The value of `field` inside `object`.
    [[nodiscard]] Common::ResultStr<Value> ReadField( const FieldInfo& field, const void* object );

    /// Stores `value` into `field` inside `object`. Refused, with the reason, when the field is ReadOnly,
    /// when the value is of another kind than the field (no conversion: converting is the caller's
    /// language rule, as for FunctionInfo::Invoke), or when an integer does not fit the field's width.
    [[nodiscard]] Common::BoolResultStr WriteField( const FieldInfo& field, void* object, const Value& value );
} // namespace Desert::Reflection
