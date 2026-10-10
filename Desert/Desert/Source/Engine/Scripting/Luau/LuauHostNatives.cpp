#include <Engine/Scripting/Luau/LuauHost.hpp>

#include <memory>
#include <span>

// THE LANGUAGE HOST'S OWN NATIVES — what is about Luau itself or about the script host, not about the engine:
// print-style logging of any values, Luau values shared between scripts (World.set/get/has), an entity object's
// liveness, destroying an entity whose scripts this host runs, calling into another entity's scripts, and the
// factory that turns a Luau function into a Reflection::Callable (Timer.after's callback). Everything the ENGINE
// offers reaches Luau through reflection (Engine/Libraries, LuauBinder) — never through a file here.

namespace Desert::Scripting
{
    namespace
    {
        // Every argument as text (tostring), tab-separated — what print() would show.
        int Log( lua_State* L )
        {
            std::string text;
            for ( int i = 1, n = lua_gettop( L ); i <= n; ++i )
            {
                std::size_t length = 0;
                const char* piece  = luaL_tolstring( L, i, &length );
                if ( i > 1 )
                    text += '\t';
                text.append( piece, length );
                lua_pop( L, 1 );
            }
            LOG_INFO( "[Lua] {}", text );
            return 0;
        }

        // World.set/get/has: Luau values shared by every script (one table for the VM's lifetime).
        int WorldVarSet( lua_State* L )
        {
            luaL_checkstring( L, 1 );
            lua_getref( L, ScriptEngine::Impl::Of( L ).WorldVars );
            lua_pushvalue( L, 1 );
            lua_pushvalue( L, 2 );
            lua_rawset( L, -3 );
            return 0;
        }
        int WorldVarGet( lua_State* L )
        {
            luaL_checkstring( L, 1 );
            lua_getref( L, ScriptEngine::Impl::Of( L ).WorldVars );
            lua_pushvalue( L, 1 );
            lua_rawget( L, -2 );
            return 1;
        }
        int WorldVarHas( lua_State* L )
        {
            WorldVarGet( L );
            lua_pushboolean( L, !lua_isnil( L, -1 ) );
            return 1;
        }

        int Valid( lua_State* L )
        {
            const std::optional<LuauEntityRef> ref = LuauBinder::ToEntity( L, 1 );
            lua_pushboolean( L, ref && ref->Registry != nullptr && ref->Registry->valid( ref->Entity ) );
            return 1;
        }

        // Removes this entity (and its subtree) from the scene; its scripts are released once the call returns.
        int Destroy( lua_State* L )
        {
            const std::optional<LuauEntityRef> ref  = LuauBinder::ToEntity( L, 1 );
            ScriptEngine::Impl&                host = ScriptEngine::Impl::Of( L );
            if ( !ref || ref->Registry == nullptr || !ref->Registry->valid( ref->Entity ) ||
                 host.Scene == nullptr )
                return 0;
            host.PendingRelease.push_back( static_cast<uint32_t>( ref->Entity ) );
            host.Scene->DestroyEntity( ECS::Entity{ ref->Entity, *ref->Registry } );
            return 0;
        }

        // Cross-entity message: target:call(fn, ...) runs `fn` with the arguments in EVERY script slot of the
        // target that defines it (an entity may run many behaviours). The mechanism behind OnInteract.
        int Call( lua_State* L )
        {
            const LuauEntityRef ref      = LuauBinder::CheckEntity( L, 1 );
            const char*         function = luaL_checkstring( L, 2 );
            ScriptEngine::Impl& host     = ScriptEngine::Impl::Of( L );
            auto                found    = host.Slots.find( static_cast<uint32_t>( ref.Entity ) );
            if ( found == host.Slots.end() )
                return 0;
            const std::vector<LuauSlot> slots = found->second; // the callee may change the map
            const int                   count = lua_gettop( L ) - 2;
            for ( LuauSlot slot : slots )
            {
                if ( slot == 0 || !host.Runtime->Defines( slot, function ) )
                    continue;
                if ( Common::BoolResultStr r = host.Runtime->CallFrom( slot, function, L, 3, count );
                     !r.IsSuccess() )
                    LOG_ERROR( "[Lua] {} error: {}", function, r.GetError() );
            }
            return 0;
        }

        /// A Luau function held by the engine: pinned in the VM's registry for as long as a copy lives, alive while
        /// the (entity, slot) that made it still runs the same load, called on that slot's thread with the world
        /// open — exactly as the slot's own OnUpdate is.
        class LuauCallable final : public Reflection::Callable::Target
        {
        public:
            LuauCallable( ScriptEngine::Impl& host, uint64_t owner, int function )
                 : m_Host( &host ), m_Lifetime( host.Lifetime ), m_Owner( owner ),
                   m_Generation( host.Generations[owner] ), m_Function( function )
            {
            }
            ~LuauCallable() override
            {
                if ( !m_Lifetime.expired() )
                    m_Host->Runtime->Unref( m_Function );
            }
            LuauCallable( const LuauCallable& )            = delete;
            LuauCallable& operator=( const LuauCallable& ) = delete;

            [[nodiscard]] bool Alive() const override
            {
                if ( m_Lifetime.expired() )
                    return false;
                const auto generation = m_Host->Generations.find( m_Owner );
                return generation != m_Host->Generations.end() && generation->second == m_Generation &&
                       Slot() != 0;
            }

            Common::BoolResultStr Call( const Reflection::Value* args, std::size_t count ) override
            {
                ScriptEngine::Impl& host     = *m_Host;
                const uint64_t      previous = host.CurrentOwner;
                host.CurrentOwner            = m_Owner; // a re-arm inherits the same (entity, slot)
                Common::BoolResultStr called = [&]
                {
                    const Core::WorldContext::Scope world( host.Context );
                    return host.Runtime->CallRef( Slot(), m_Function,
                                                  std::span<const Reflection::Value>( args, count ) );
                }();
                host.CurrentOwner = previous;
                host.Settle();
                return called;
            }

        private:
            LuauSlot Slot() const
            {
                return m_Host->SlotOf( static_cast<uint32_t>( m_Owner >> 32 ),
                                       static_cast<uint32_t>( m_Owner & 0xFFFFFFFFu ) );
            }

            ScriptEngine::Impl* m_Host;
            std::weak_ptr<int>  m_Lifetime;
            uint64_t            m_Owner;
            uint32_t            m_Generation;
            int                 m_Function;
        };

        std::optional<Reflection::Callable> MakeCallable( lua_State* L, int index, std::string& why )
        {
            if ( !lua_isfunction( L, index ) )
            {
                why = "expected a function";
                return std::nullopt;
            }
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            return Reflection::Callable( std::make_shared<LuauCallable>( host, host.CurrentOwner, lua_ref( L, index ) ) );
        }

        /// The global table `name` (the reflected library's, when one is bound under it), created when absent.
        void OpenGlobalTable( lua_State* L, const char* name )
        {
            lua_getglobal( L, name );
            if ( lua_istable( L, -1 ) )
                return;
            lua_pop( L, 1 );
            lua_newtable( L );
            lua_pushvalue( L, -1 );
            lua_setglobal( L, name );
        }
    } // namespace

    void RegisterHostNatives( lua_State* L )
    {
        static constexpr LuauBinder::CallableFactory kFactory = &MakeCallable;
        LuauBinder::SetCallableFactory( L, &kFactory );

        lua_pushcfunction( L, &Log, "log" );
        lua_setglobal( L, "log" );

        OpenGlobalTable( L, "World" );
        for ( const luaL_Reg& entry :
              { luaL_Reg{ "set", &WorldVarSet }, luaL_Reg{ "get", &WorldVarGet }, luaL_Reg{ "has", &WorldVarHas } } )
        {
            lua_pushcfunction( L, entry.func, entry.name );
            lua_setfield( L, -2, entry.name );
        }
        lua_pop( L, 1 );

        for ( const luaL_Reg& method :
              { luaL_Reg{ "valid", &Valid }, luaL_Reg{ "destroy", &Destroy }, luaL_Reg{ "call", &Call } } )
            LuauBinder::SetEntityMethod( L, method.name, method.func );
    }
} // namespace Desert::Scripting
