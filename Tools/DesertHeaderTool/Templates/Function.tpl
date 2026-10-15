                    .Function( ::Desert::Reflection::MakeFunction<&T::{{ fn.name }}>( "{{ fn.name }}", "{{ t.registryName }}", "{{ fn.returnType }}", std::array<::Desert::Reflection::ParamSpelling, {{ fn.paramCount }}>{ {% for p in fn.params %}::Desert::Reflection::ParamSpelling{ "{{ p.name }}", "{{ p.cppType }}"{% if p.hasDefault %}, +[]() -> ::Desert::Reflection::Value { using P = ::Desert::Reflection::ParamType<&T::{{ fn.name }}, {{ p.index }}>; return ::Desert::Reflection::ValueTraits<P>::To( P{ {{ p.defaultInit }} } ); }{% endif %} }, {% endfor %}}, ::Desert::Reflection::FunctionMetadata{ .ScriptCallable = {{ fn.scriptCallable }}, .Category = "{{ fn.category }}", .Tooltip = "{{ fn.tooltip }}", .ScriptName = "{{ fn.scriptName }}", .ScriptMethod = {{ fn.scriptMethod }} } ) )
{#- One FUNCTION(...), one output line. Only the names and the attributes are the tool's; the kinds, static,
    const and the call itself are deduced from &T::name by MakeFunction (Engine/Reflection/FunctionThunk.hpp),
    which also static_asserts that paramCount is the count the compiler sees. #}
{#- A parameter's C++ default (UE's CPP_Default_) is compiled, not parsed: a lambda converts it to the
    parameter's own type and then to a Value once, at registration, so a caller that leaves the argument out
    gets exactly what C++ would have passed. #}
