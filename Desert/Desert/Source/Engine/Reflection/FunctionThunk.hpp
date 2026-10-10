#pragma once

// THE THUNKS DesertHeaderTool EMITS FOR FUNCTION(...) — UE's UHT `execFoo` thunks, written once as templates.
//
// UHT spells every parameter type into generated text because UE's C++ cannot deduce them; C++20 can. So the
// tool emits only what the compiler cannot know — the parameters' NAMES and the attributes — and one line
//
//     .Function( MakeFunction<&T::SetIntensity>( "SetIntensity", "LightData", "void", { ParamSpelling{
//     "intensity", "float" } }, meta ) )
//
// and everything about the signature (kinds, static, const, the call) comes from `&T::SetIntensity` itself.
// The scanner reading a header and the compiler reading the same header cannot disagree on a type, and on the
// one thing both state, the parameter count, a disagreement is a static_assert in the generated file.

#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Reflection/Value.hpp>

#include <entt/entt.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Desert::Reflection
{
    // ------------------------------------------------------------------ C++ type <-> Value

    /// How one C++ parameter/result type travels as a Value. A type with no specialisation is a compile error
    /// in the generated file naming the type — never a silently skipped function.
    template <typename T, typename Enable = void>
    struct ValueTraits
    {
        static_assert( sizeof( T ) == 0, "FUNCTION(...): this parameter/result type has no Value kind "
                                         "(Engine/Reflection/FunctionThunk.hpp, ValueTraits)" );
    };

    /// Whether `v` can become an X: the kind is X's (anything, for a Value parameter) and, for a list, a record or
    /// a struct, every element or field fits too. A thunk refuses what does not fit before it unpacks.
    template <typename X>
    bool KindFits( const Value& v )
    {
        using Traits = ValueTraits<X>;
        if constexpr ( Traits::Kind == FieldType::Any )
            return true;
        else
        {
            if ( v.Type() != Traits::Kind )
                return false;
            if constexpr ( requires { Traits::Fits( v ); } )
                return Traits::Fits( v );
            else
                return true;
        }
    }

    /// REFLECT( ScriptStruct ) types — UE's USTRUCT(BlueprintType). DesertHeaderTool specialises this for each one
    /// in its module's generated file, before the FUNCTIONs that take or return it: the registry name and the
    /// member list `std::tuple{ StructMember{ "Field", &T::Field }, ... }`. Unspecialised = not a script struct.
    template <typename T>
    struct ReflectedStruct
    {
    };

    template <typename C, typename M>
    struct StructMember
    {
        const char* Name;
        M C::*      Pointer;
    };
    template <typename C, typename M>
    StructMember( const char*, M C::* ) -> StructMember<C, M>;

    template <typename T>
    inline constexpr bool kIsReflectedStruct = requires { ReflectedStruct<T>::Name; };

    template <>
    struct ValueTraits<bool>
    {
        static constexpr FieldType Kind = FieldType::Bool;
        static bool                From( const Value& v )
        {
            return *v.Get<bool>();
        }
        static Value To( bool v )
        {
            return Value::Bool( v );
        }
    };

    template <typename T>
    struct ValueTraits<T,
                       std::enable_if_t<std::is_integral_v<T> && std::is_signed_v<T> && !std::is_same_v<T, bool>>>
    {
        static constexpr FieldType Kind = FieldType::Int;
        static bool                Fits( const Value& v )
        {
            const std::int64_t x = *v.Get<std::int64_t>();
            return x >= std::numeric_limits<T>::min() && x <= std::numeric_limits<T>::max();
        }
        static T From( const Value& v )
        {
            return static_cast<T>( *v.Get<std::int64_t>() );
        }
        static Value To( T v )
        {
            return Value::Int( v );
        }
    };

    template <typename T>
    struct ValueTraits<
         T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T> && !std::is_same_v<T, bool>>>
    {
        static constexpr FieldType Kind = FieldType::UInt;
        static bool                Fits( const Value& v )
        {
            return *v.Get<std::uint64_t>() <= std::numeric_limits<T>::max();
        }
        static T From( const Value& v )
        {
            return static_cast<T>( *v.Get<std::uint64_t>() );
        }
        static Value To( T v )
        {
            return Value::UInt( v );
        }
    };

    template <>
    struct ValueTraits<float>
    {
        static constexpr FieldType Kind = FieldType::Float;
        static float               From( const Value& v )
        {
            return *v.Get<float>();
        }
        static Value To( float v )
        {
            return Value::Float( v );
        }
    };

    template <>
    struct ValueTraits<double>
    {
        static constexpr FieldType Kind = FieldType::Double;
        static double              From( const Value& v )
        {
            return *v.Get<double>();
        }
        static Value To( double v )
        {
            return Value::Double( v );
        }
    };

    template <>
    struct ValueTraits<std::string>
    {
        static constexpr FieldType Kind = FieldType::String;
        static const std::string&  From( const Value& v )
        {
            return *v.Get<std::string>();
        }
        static Value To( std::string v )
        {
            return Value::String( std::move( v ) );
        }
    };

    template <>
    struct ValueTraits<glm::vec2>
    {
        static constexpr FieldType Kind = FieldType::Vec2;
        static glm::vec2           From( const Value& v )
        {
            const auto& a = *v.Get<Value::Float2>();
            return { a[0], a[1] };
        }
        static Value To( const glm::vec2& v )
        {
            return Value::Vec2( { v.x, v.y } );
        }
    };

    template <>
    struct ValueTraits<glm::vec3>
    {
        static constexpr FieldType Kind = FieldType::Vec3;
        static glm::vec3           From( const Value& v )
        {
            const auto& a = *v.Get<Value::Float3>();
            return { a[0], a[1], a[2] };
        }
        static Value To( const glm::vec3& v )
        {
            return Value::Vec3( { v.x, v.y, v.z } );
        }
    };

    template <>
    struct ValueTraits<glm::vec4>
    {
        static constexpr FieldType Kind = FieldType::Vec4;
        static glm::vec4           From( const Value& v )
        {
            const auto& a = *v.Get<Value::Float4>();
            return { a[0], a[1], a[2], a[3] };
        }
        static Value To( const glm::vec4& v )
        {
            return Value::Vec4( { v.x, v.y, v.z, v.w } );
        }
    };

    template <typename T>
    struct ValueTraits<T, std::enable_if_t<std::is_enum_v<T>>>
    {
        static constexpr FieldType Kind = FieldType::Enum;
        static T                   From( const Value& v )
        {
            return static_cast<T>( v.Get<Value::EnumBits>()->Bits );
        }
        static Value To( T v )
        {
            return Value::Enum( static_cast<std::int64_t>( v ) );
        }
    };

    /// An entity travels as its id, a UInt (as an asset handle does): the identity a language holds and hands
    /// back; entt::null is the all-ones id. Spelled before the enum rule so an entity is never an Enum.
    template <>
    struct ValueTraits<entt::entity>
    {
        using Id                        = std::underlying_type_t<entt::entity>;
        static constexpr FieldType Kind = FieldType::UInt;
        static bool                Fits( const Value& v )
        {
            return *v.Get<std::uint64_t>() <= std::numeric_limits<Id>::max();
        }
        static entt::entity From( const Value& v )
        {
            return static_cast<entt::entity>( static_cast<Id>( *v.Get<std::uint64_t>() ) );
        }
        static Value To( entt::entity v )
        {
            return Value::UInt( static_cast<std::uint64_t>( static_cast<Id>( v ) ) );
        }
    };

    /// A Value itself is the Any kind (Godot's Variant): the function receives the argument as the caller gave
    /// it and reads its Type(); returned, a Value travels as what it holds (nothing = the language's nil).
    template <>
    struct ValueTraits<Value>
    {
        static constexpr FieldType Kind = FieldType::Any;
        static Value               From( const Value& v )
        {
            return v;
        }
        static Value To( Value v )
        {
            return v;
        }
    };

    /// A list travels as an Array, each element as its own kind (a list of entities as entities).
    template <typename E>
    struct ValueTraits<std::vector<E>>
    {
        static constexpr FieldType Kind = FieldType::Array;
        using Element                   = E;
        static bool Fits( const Value& v )
        {
            for ( const Value& item : *v.Get<Value::Array>() )
                if ( !KindFits<E>( item ) )
                    return false;
            return true;
        }
        static std::vector<E> From( const Value& v )
        {
            std::vector<E> out;
            out.reserve( v.Get<Value::Array>()->size() );
            for ( const Value& item : *v.Get<Value::Array>() )
                out.push_back( ValueTraits<E>::From( item ) );
            return out;
        }
        static Value To( const std::vector<E>& items )
        {
            Value::Array out;
            out.reserve( items.size() );
            for ( const E& item : items )
                out.push_back( ValueTraits<E>::To( item ) );
            return Value::MakeArray( std::move( out ) );
        }
    };

    /// A string-keyed map travels as a Map, each value as its own kind.
    template <typename E>
    struct ValueTraits<std::map<std::string, E>>
    {
        static constexpr FieldType Kind = FieldType::Map;
        using Element                   = E;
        static bool Fits( const Value& v )
        {
            for ( const Value& item : v.Get<Value::Map>()->Values )
                if ( !KindFits<E>( item ) )
                    return false;
            return true;
        }
        static std::map<std::string, E> From( const Value& v )
        {
            const Value::Map&        map = *v.Get<Value::Map>();
            std::map<std::string, E> out;
            for ( std::size_t i = 0; i < map.Size(); ++i )
                out.emplace( map.Keys[i], ValueTraits<E>::From( map.Values[i] ) );
            return out;
        }
        static Value To( const std::map<std::string, E>& items )
        {
            Value::Map out;
            for ( const auto& [key, item] : items )
                out.Set( key, ValueTraits<E>::To( item ) );
            return Value::MakeMap( std::move( out ) );
        }
    };

    /// An open record (a Lua table of mixed fields) is a Map of Values: the callee reads each field's own kind.
    template <>
    struct ValueTraits<Value::Map>
    {
        static constexpr FieldType Kind = FieldType::Map;
        using Element                   = Value;
        static const Value::Map& From( const Value& v )
        {
            return *v.Get<Value::Map>();
        }
        static Value To( Value::Map v )
        {
            return Value::MakeMap( std::move( v ) );
        }
    };

    /// A callable a language handed over (UE's delegate): kept and called later by the engine.
    template <>
    struct ValueTraits<Callable>
    {
        static constexpr FieldType Kind = FieldType::Callable;
        static const Callable&     From( const Value& v )
        {
            return *v.Get<Callable>();
        }
        static Value To( Callable v )
        {
            return Value::MakeCallable( std::move( v ) );
        }
    };

    /// A REFLECT( ScriptStruct ) instance travels as a Struct: its registry name and each member as its own kind
    /// (UE's UScriptStruct read through its FProperty list). A field the caller leaves out keeps the member's
    /// default; a field the type does not have, or of another kind, is refused before the call.
    template <typename T>
    struct ValueTraits<T, std::enable_if_t<kIsReflectedStruct<T>>>
    {
        static constexpr FieldType Kind = FieldType::Struct;

        static bool Fits( const Value& v )
        {
            const Value::StructData& data = *v.Get<Value::StructData>();
            if ( data.Type != ReflectedStruct<T>::Name )
                return false;
            for ( std::size_t i = 0; i < data.Fields.Size(); ++i )
            {
                const bool fits = std::apply(
                     [&]( const auto&... member )
                     {
                         bool found = false;
                         bool ok    = false;
                         ( ( !found && data.Fields.Keys[i] == member.Name
                                  ? ( found = true,
                                      ok    = KindFits<std::remove_cvref_t<decltype( std::declval<T&>().*
                                                                                   member.Pointer )>>(
                                           data.Fields.Values[i] ) )
                                  : false ),
                           ... );
                         return found && ok;
                     },
                     ReflectedStruct<T>::Members );
                if ( !fits )
                    return false;
            }
            return true;
        }
        static T From( const Value& v )
        {
            const Value::StructData& data = *v.Get<Value::StructData>();
            T                        out{};
            std::apply(
                 [&]( const auto&... member )
                 {
                     ( ( [&]
                         {
                             using M = std::remove_cvref_t<decltype( out.*member.Pointer )>;
                             if ( const Value* field = data.Fields.Find( member.Name ) )
                                 out.*member.Pointer = ValueTraits<M>::From( *field );
                         }() ),
                       ... );
                 },
                 ReflectedStruct<T>::Members );
            return out;
        }
        static Value To( const T& v )
        {
            Value::Map fields;
            std::apply(
                 [&]( const auto&... member )
                 {
                     ( fields.Set( member.Name,
                                   ValueTraits<std::remove_cvref_t<decltype( v.*member.Pointer )>>::To(
                                        v.*member.Pointer ) ),
                       ... );
                 },
                 ReflectedStruct<T>::Members );
            return Value::MakeStruct( ReflectedStruct<T>::Name, std::move( fields ) );
        }
    };

    /// The kind of every element of X (a list or a record), or Unknown.
    template <typename X>
    constexpr FieldType ElementKindOf()
    {
        if constexpr ( requires { typename ValueTraits<X>::Element; } )
            return ValueTraits<typename ValueTraits<X>::Element>::Kind;
        else
            return FieldType::Unknown;
    }

    /// The registry name of X's script struct — X itself, or its element — or "".
    template <typename X>
    const char* StructNameOf()
    {
        if constexpr ( kIsReflectedStruct<X> )
            return ReflectedStruct<X>::Name;
        else if constexpr ( requires { typename ValueTraits<X>::Element; } )
            return StructNameOf<typename ValueTraits<X>::Element>();
        else
            return "";
    }

    /// A parameter is taken by value or by const reference; a non-const reference would be an OUT parameter,
    /// which this layer does not have (a second result is a second return, REMAINDER of SCR-API-1).
    template <typename A>
    using ParamValueType = std::remove_cvref_t<A>;

    template <typename A>
    inline constexpr bool kIsPassableParam =
         !std::is_lvalue_reference_v<A> || std::is_const_v<std::remove_reference_t<A>>;

    /// A function returning Common::BoolResultStr answers in Lua's idiom for a failure the caller handles: two
    /// results, Ok (Bool) and Error (String, empty on success) — never a call error.
    template <typename R>
    inline constexpr bool kIsOutcome = std::is_same_v<std::remove_cvref_t<R>, Common::BoolResultStr>;

    // ------------------------------------------------------------------ signature of &T::F

    template <typename F>
    struct Signature;

    template <typename R, typename... A>
    struct Signature<R ( * )( A... )>
    {
        using Return                   = R;
        using Args                     = std::tuple<A...>;
        using Self                     = void;
        static constexpr bool IsStatic = true;
        static constexpr bool IsConst  = false;
    };
    template <typename R, typename... A>
    struct Signature<R ( * )( A... ) noexcept> : Signature<R ( * )( A... )>
    {
    };

    template <typename R, typename C, typename... A>
    struct Signature<R ( C::* )( A... )>
    {
        using Return                   = R;
        using Args                     = std::tuple<A...>;
        using Self                     = C;
        static constexpr bool IsStatic = false;
        static constexpr bool IsConst  = false;
    };
    template <typename R, typename C, typename... A>
    struct Signature<R ( C::* )( A... ) noexcept> : Signature<R ( C::* )( A... )>
    {
    };

    template <typename R, typename C, typename... A>
    struct Signature<R ( C::* )( A... ) const>
    {
        using Return                   = R;
        using Args                     = std::tuple<A...>;
        using Self                     = const C;
        static constexpr bool IsStatic = false;
        static constexpr bool IsConst  = true;
    };
    template <typename R, typename C, typename... A>
    struct Signature<R ( C::* )( A... ) const noexcept> : Signature<R ( C::* )( A... ) const>
    {
    };

    // ------------------------------------------------------------------ the thunk

    namespace Detail
    {
        template <typename A>
        bool ArgFits( const Value& v )
        {
            return KindFits<ParamValueType<A>>( v );
        }

        template <auto F, std::size_t... I>
        Common::BoolResultStr Call( void* self, const Value* args, Value* rets, std::index_sequence<I...> )
        {
            using S    = Signature<decltype( F )>;
            using Args = typename S::Args;

            // Range: an Int Value that does not fit an int32 parameter is refused, not wrapped.
            using FitCheck                                       = bool ( * )( const Value& );
            constexpr std::array<FitCheck, sizeof...( I )> kFits = { &ArgFits<std::tuple_element_t<I, Args>>... };
            for ( std::size_t i = 0; i < kFits.size(); ++i )
                if ( !kFits[i]( args[i] ) )
                    return Common::MakeError<bool>( std::format(
                         "argument {} is outside its parameter's range, or an element or field of it is not of "
                         "its kind",
                         i ) );

            const auto invoke = [&]() -> decltype( auto )
            {
                if constexpr ( S::IsStatic )
                    return F( ValueTraits<ParamValueType<std::tuple_element_t<I, Args>>>::From( args[I] )... );
                else
                    return ( static_cast<typename S::Self*>( self )->*F )(
                         ValueTraits<ParamValueType<std::tuple_element_t<I, Args>>>::From( args[I] )... );
            };
            if constexpr ( std::is_void_v<typename S::Return> )
                invoke();
            else if constexpr ( kIsOutcome<typename S::Return> )
            {
                const Common::BoolResultStr outcome = invoke();
                rets[0]                             = Value::Bool( outcome.IsSuccess() );
                rets[1] = Value::String( outcome.IsSuccess() ? std::string() : outcome.GetError() );
            }
            else
                rets[0] = ValueTraits<std::remove_cvref_t<typename S::Return>>::To( invoke() );
            return Common::MakeSuccess( true );
        }
    } // namespace Detail

    /// The generated thunk of &T::F (FunctionThunk in ReflectionTypes.hpp).
    template <auto F>
    Common::BoolResultStr Thunk( void* self, const Value* args, Value* rets )
    {
        using S = Signature<decltype( F )>;
        return Detail::Call<F>( self, args, rets,
                                std::make_index_sequence<std::tuple_size_v<typename S::Args>>{} );
    }

    // ------------------------------------------------------------------ the record

    /// What the header tool read for one parameter: its name, and its spelling (kept for diagnostics and for a
    /// binding generator; the KIND is the compiler's, never parsed from this text).
    struct ParamSpelling
    {
        const char* Name;
        const char* TypeName;
        Value ( *Default )() = nullptr; // the header's default expression as a Value (Function.tpl), or none
    };

    /// The I-th parameter type of &T::F, as a Value carries it (the generated default of that parameter).
    template <auto F, std::size_t I>
    using ParamType = ParamValueType<std::tuple_element_t<I, typename Signature<decltype( F )>::Args>>;

    namespace Detail
    {
        template <typename Args, std::size_t N, std::size_t... I>
        std::vector<ParamInfo> Params( [[maybe_unused]] const std::array<ParamSpelling, N>& spelling,
                                       std::index_sequence<I...> )
        {
            static_assert( ( kIsPassableParam<std::tuple_element_t<I, Args>> && ... ),
                           "FUNCTION(...): a non-const reference parameter is an out parameter, which "
                           "reflected functions do not have" );
            std::vector<ParamInfo> params = {
                 ParamInfo{ .Name        = spelling[I].Name,
                            .Type        = ValueTraits<ParamValueType<std::tuple_element_t<I, Args>>>::Kind,
                            .TypeName    = spelling[I].TypeName,
                            .ElementType = ElementKindOf<ParamValueType<std::tuple_element_t<I, Args>>>(),
                            .StructName  = StructNameOf<ParamValueType<std::tuple_element_t<I, Args>>>() }... };
            for ( std::size_t i = 0; i < N; ++i )
                if ( spelling[i].Default != nullptr )
                    params[i].Default = std::make_shared<const Value>( spelling[i].Default() );
            return params;
        }
    } // namespace Detail

    /// The FunctionInfo of &T::F, as the generated reflection registers it.
    template <auto F, std::size_t N>
    FunctionInfo MakeFunction( const char* name, const char* owner, const char* returnSpelling,
                               const std::array<ParamSpelling, N>& params, FunctionMetadata meta )
    {
        using S    = Signature<decltype( F )>;
        using Args = typename S::Args;
        static_assert( std::tuple_size_v<Args> == N,
                       "FUNCTION(...): the header tool read another parameter count than the compiler sees" );

        FunctionInfo info;
        info.Name   = name;
        info.Owner  = owner;
        info.Params = Detail::Params<Args>( params, std::make_index_sequence<N>{} );
        using R = std::remove_cvref_t<typename S::Return>;
        if constexpr ( kIsOutcome<R> )
        {
            info.Returns.push_back( ParamInfo{ .Name = "Ok", .Type = FieldType::Bool, .TypeName = returnSpelling } );
            info.Returns.push_back(
                 ParamInfo{ .Name = "Error", .Type = FieldType::String, .TypeName = returnSpelling } );
        }
        else if constexpr ( !std::is_void_v<R> )
            info.Returns.push_back( ParamInfo{ .Name        = "ReturnValue",
                                               .Type        = ValueTraits<R>::Kind,
                                               .TypeName    = returnSpelling,
                                               .ElementType = ElementKindOf<R>(),
                                               .StructName  = StructNameOf<R>() } );
        info.IsStatic = S::IsStatic;
        info.IsConst  = S::IsConst;
        info.Meta     = std::move( meta );
        info.Thunk    = &Thunk<F>;
        return info;
    }

    // ------------------------------------------------------------------ events

    template <typename F>
    struct EventSignature
    {
        static_assert( sizeof( F ) == 0,
                       "EVENT(...): the alias must name a function type, `using OnX = void( ... );`" );
    };
    template <typename... A>
    struct EventSignature<void( A... )>
    {
        using Args = std::tuple<A...>;
    };

    /// The EventInfo of the alias `F` (EVENT(...) in ReflectionMacros.hpp), as the generated reflection registers
    /// it: the names are the tool's, the kinds the compiler's — exactly as MakeFunction.
    template <typename F, std::size_t N>
    EventInfo MakeEvent( const char* name, const char* owner, const std::array<ParamSpelling, N>& params,
                         EventMetadata meta )
    {
        using Args = typename EventSignature<F>::Args;
        static_assert( std::tuple_size_v<Args> == N,
                       "EVENT(...): the header tool read another parameter count than the compiler sees" );
        EventInfo info;
        info.Name   = name;
        info.Owner  = owner;
        info.Params = Detail::Params<Args>( params, std::make_index_sequence<N>{} );
        info.Meta   = std::move( meta );
        return info;
    }

    /// The payload of one firing of `F`, packed as its subscribers receive it — the arguments converted by the
    /// same ValueTraits a reflected call uses, so a C++ broadcaster cannot send another signature than declared.
    template <typename F, typename... A>
    std::array<Value, sizeof...( A )> EventPayload( A&&... args )
    {
        using Args = typename EventSignature<F>::Args;
        static_assert( std::tuple_size_v<Args> == sizeof...( A ),
                       "EventPayload: another argument count than the event's" );
        return [&]<std::size_t... I>( std::index_sequence<I...> )
        {
            return std::array<Value, sizeof...( A )>{
                 ValueTraits<ParamValueType<std::tuple_element_t<I, Args>>>::To(
                      static_cast<ParamValueType<std::tuple_element_t<I, Args>>>( std::forward<A>( args ) ) )... };
        }( std::index_sequence_for<A...>{} );
    }
} // namespace Desert::Reflection
