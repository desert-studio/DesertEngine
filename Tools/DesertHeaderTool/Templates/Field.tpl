                    .Field( FieldInfo{ .Name = "{{ f.name }}", .Type = FieldType::{{ f.fieldType }}, .Offset = offsetof( T, {{ f.name }} ), .Size = ::Desert::Reflection::FieldFootprint<decltype( T::{{ f.name }} )>(), .TypeName = "{{ f.cppType }}", .Meta = {% include "Metadata.tpl" %}
{#- One reflected field, one output line. `.Size` is FieldFootprint<decltype(...)>() and not sizeof(...): see
    the note on that function. A container's element type is never re-spelled: decltype( T::field ) names the
    exact vector, and an asset handle is stored as its 64-bit id (both halves in ReflectionSerializer.hpp,
    where the reader applies the wrong-type rule). #}
{%- if f.enumValues %}, .EnumValues = { {% for v in f.enumValues %}EnumValue{ "{{ v.name }}", {{ v.value }} }, {% endfor %}}{% endif %}
{%- if f.isContainer %}, .IsContainer = true, .SerializeContainer = ::Desert::Reflection::WriteContainer<decltype( T::{{ f.name }} ){% if f.elemFieldType == "AssetHandle" %}, std::uint64_t{% endif %}>, .DeserializeContainer = ::Desert::Reflection::ReadContainer<decltype( T::{{ f.name }} ){% if f.elemFieldType == "AssetHandle" %}, std::uint64_t{% endif %}>{% endif %} } )
