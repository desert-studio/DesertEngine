#include <Engine/Core/Serialize/ComponentSerializerKit.hpp>

// The Script block: the .lua reference + the exposed-property values.

namespace Desert::Core::Serialize::Builtins
{
    namespace
    {

        // ScriptComponent has no reflected data block (reflection can't do std::string/variant lists), so it
        // gets a manual serializer via a reflect-cpp mirror: the .lua reference + the exposed-property values.
        //
        // A property holds the value of its kind only: Number/Bool/Str are always written (the v16 shape), the
        // vector (Color, Vector), the reference (Asset handle, Entity UUID) and an Array's items only when the
        // kind has them — a file without them is a file without such properties, not an older format.
        struct ScriptValueSer
        {
            int                                  Type   = 0;
            double                               Number = 0.0;
            bool                                 Bool   = false;
            std::string                          Str;
            std::optional<std::array<double, 3>> Vec;
            std::optional<std::uint64_t>         Ref;
        };
        struct ScriptPropSer
        {
            std::string                                Name;
            int                                        Type   = 0;
            double                                     Number = 0.0;
            bool                                       Bool   = false;
            std::string                                Str;
            std::optional<std::array<double, 3>>       Vec;
            std::optional<std::uint64_t>               Ref;
            std::optional<int>                         ElementType;
            std::optional<std::vector<ScriptValueSer>> Items;
        };

        bool HasVec( Scripting::PropertyType type )
        {
            return type == Scripting::PropertyType::Color || type == Scripting::PropertyType::Vector;
        }
        bool HasRef( Scripting::PropertyType type )
        {
            return type == Scripting::PropertyType::Asset || type == Scripting::PropertyType::Entity;
        }

        ScriptValueSer SaveValue( const Scripting::ScriptValue& v )
        {
            ScriptValueSer out{ static_cast<int>( v.Type ), v.Number, v.Bool, v.Str, std::nullopt, std::nullopt };
            if ( HasVec( v.Type ) )
                out.Vec = v.Vec;
            if ( HasRef( v.Type ) )
                out.Ref = v.Ref;
            return out;
        }

        Scripting::ScriptValue LoadValue( const ScriptValueSer& v )
        {
            Scripting::ScriptValue out;
            out.Type   = static_cast<Scripting::PropertyType>( v.Type );
            out.Number = v.Number;
            out.Bool   = v.Bool;
            out.Str    = v.Str;
            out.Vec    = v.Vec.value_or( std::array<double, 3>{} );
            out.Ref    = v.Ref.value_or( 0 );
            return out;
        }
        // `ScriptKey` and not `Path`, because the value stopped being a path at scene v16 (I9): it is the
        // root-tagged key ScriptSlot documents. Renaming the JSON field with the value is the point — a
        // key called `Path` reads as something you may hand to std::filesystem, which is exactly the
        // mistake the migration exists to undo, and the loader refuses anything below v16 anyway so there
        // is no file in which both spellings can be present.
        struct ScriptSlotSer
        {
            std::string                ScriptKey;
            std::vector<ScriptPropSer> Props;
        };
        // One entity runs a LIST of scripts (single-script legacy format removed).
        struct ScriptCompSer
        {
            std::optional<std::vector<ScriptSlotSer>> Scripts;
        };

        // Rebuild a ScriptSlot's properties from its serialized form.
        auto loadProps = []( const std::vector<ScriptPropSer>& src )
        {
            std::vector<Scripting::ScriptProperty> out;
            for ( const auto& p : src )
            {
                Scripting::ScriptProperty prop;
                static_cast<Scripting::ScriptValue&>( prop ) =
                     LoadValue( { p.Type, p.Number, p.Bool, p.Str, p.Vec, p.Ref } );
                prop.Name        = p.Name;
                prop.ElementType = static_cast<Scripting::PropertyType>( p.ElementType.value_or( 0 ) );
                if ( p.Items )
                    for ( const ScriptValueSer& item : *p.Items )
                        prop.Items.push_back( LoadValue( item ) );
                out.push_back( std::move( prop ) );
            }
            return out;
        };

    } // namespace

        ComponentSerializer MakeScript()
        {
            ComponentSerializer s;
            s.Key = "Script";
            s.Has = []( ECS::Entity e ) { return e.HasComponent<ECS::ScriptComponent>(); };

            s.Serialize = []( ECS::Entity e, const Assets::AssetManager& ) -> Common::Json::Value
            {
                const auto&                sc = e.GetComponent<ECS::ScriptComponent>();
                ScriptCompSer              ser;
                std::vector<ScriptSlotSer> slots;
                for ( const auto& slot : sc.Scripts )
                {
                    ScriptSlotSer ss;
                    ss.ScriptKey = slot.ScriptKey;
                    for ( const auto& p : slot.Properties )
                    {
                        const ScriptValueSer value = SaveValue( p );
                        ScriptPropSer        prop{ p.Name,    value.Type, value.Number, value.Bool,  value.Str,
                                            value.Vec, value.Ref,  std::nullopt, std::nullopt };
                        if ( p.Type == Scripting::PropertyType::Array )
                        {
                            prop.ElementType = static_cast<int>( p.ElementType );
                            prop.Items.emplace();
                            for ( const Scripting::ScriptValue& item : p.Items )
                                prop.Items->push_back( SaveValue( item ) );
                        }
                        ss.Props.push_back( std::move( prop ) );
                    }
                    slots.push_back( std::move( ss ) );
                }
                ser.Scripts = std::move( slots );
                return Common::Json::FromStruct( ser );
            };

            s.Deserialize = []( ECS::Entity e, const Common::Json::Node& g, const Assets::AssetManager&,
                                Common::Json::Issues& issues )
            {
                auto ser = ReadBlock<ScriptCompSer>( g, issues );
                if ( !ser )
                    return;
                auto& sc = e.HasComponent<ECS::ScriptComponent>() ? e.GetComponent<ECS::ScriptComponent>()
                                                                  : e.AddComponent<ECS::ScriptComponent>();
                sc.Scripts.clear();
                if ( ser->Scripts )
                {
                    for ( const auto& ss : *ser->Scripts )
                    {
                        ECS::ScriptSlot slot;
                        slot.ScriptKey  = ss.ScriptKey;
                        slot.Started    = false;
                        slot.Properties = loadProps( ss.Props );
                        sc.Scripts.push_back( std::move( slot ) );
                    }
                }
            };
            return s;
        }

        // THE THREE `Ensure*Registered` HELPERS THAT STOOD HERE NOW LIVE IN
        // Engine/Runtime/Services/AssetServiceRegistration.hpp, unchanged in behaviour.
        //
        // They moved because two OTHER files had independently written the same find-else-create and each
        // got a different part of it wrong (the import's reuse path registered nothing; the thumbnail's
        // reuse path registered nothing AND then blamed the consequence on the wrong cause). A rule
        // enforced by three careful copies is the shape Ф5 paid forty-four call sites for; this one is
        // enforced by there being one implementation to call.
} // Desert::Core::Serialize::Builtins
