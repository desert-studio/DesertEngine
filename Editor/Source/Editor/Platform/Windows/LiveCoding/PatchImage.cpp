#include "PatchImage.hpp"

#include "FunctionHotpatch.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <string>
#include <unordered_map>
#include <utility>

// The in-memory linker of a Live Coding patch (Live++ model). The objects of the recompiled sources are laid out
// in one allocation near the image they patch; every symbol resolves either into that allocation or into the image
// as it runs (its /MAP, its import table, the exports of the other loaded images). Functions carrying edited code
// are laid out new; header and template code that carries none keeps calling the image's copy, so a function-local
// static inside it stays one object; writable data the image or an earlier patch has stays there; vtables, RTTI
// and unchanged constants stay the image's; edited constants are the new ones.
namespace Desert::Editor::LiveCoding
{
    namespace
    {
        constexpr std::uint32_t  kScnCode           = 0x00000020;
        constexpr std::uint32_t  kScnLnkInfo        = 0x00000200;
        constexpr std::uint32_t  kScnLnkRemove      = 0x00000800;
        constexpr std::uint32_t  kScnAlignMask      = 0x00F00000;
        constexpr std::uint32_t  kScnMemExecute     = 0x20000000;
        constexpr std::uint32_t  kScnMemWrite       = 0x80000000;
        constexpr std::uint8_t   kClassExternal     = 2;
        constexpr std::uint8_t   kClassStatic       = 3;
        constexpr std::uint8_t   kClassLabel        = 6;
        constexpr std::uint8_t   kClassWeakExternal = 105;
        constexpr std::uint32_t  kNoSymbol          = ~0u;
        constexpr std::uint32_t  kCodePadding       = 5;  // the E9 rel32 a later patch writes before an entry
        constexpr std::size_t    kThunkSize         = 16; // FF 25 00000000 (jmp [rip]) + 8-byte target, padded
        constexpr std::uintptr_t kReach             = 0x7FFF0000u - ( 16u << 20 ); // rel32 reach, 16 MB margin

        enum class Kind : std::uint8_t
        {
            Discard,  // .debug$*, .drectve, linker info, CFG tables
            Code,     // .text$*
            ReadOnly, // .rdata, .xdata
            PData,    // .pdata: RUNTIME_FUNCTION entries
            Data,     // .data, .bss
            Tls,      // .tls$: thread_local templates, only ever resolved into the image
            CrtInit,  // .CRT$XC*: dynamic initialiser tables, read here, never laid out
            Ignored,  // other .CRT$ tables (TLS callbacks, pre-/post-terminators): the image's own stay registered
        };

        enum class Region : std::uint8_t
        {
            Code,
            ReadOnly,
            PData,
            Data,
            Count
        };

        enum class Where : std::uint8_t
        {
            Undecided,
            Patch, // defined by DefObject/DefSymbol, laid out in the patch
            Image, // Address: in the image or another loaded image
            Slot,  // Address: an export the image does not import; reached through a patch-owned IAT slot
        };

        struct Decision
        {
            Where          Place     = Where::Undecided;
            bool           Function  = false;
            std::uint32_t  DefObject = 0;
            std::uint32_t  DefSymbol = kNoSymbol; // the defining symbol in the patch objects, if any
            std::uintptr_t Address   = 0;
        };

        Kind Classify( const Coff::Section& section )
        {
            const std::string_view name( section.Name );
            if ( ( section.Characteristics & ( kScnLnkInfo | kScnLnkRemove ) ) != 0 ||
                 name.starts_with( ".debug$" ) || name.starts_with( ".drectve" ) || name.starts_with( ".llvm" ) ||
                 name.starts_with( ".chks64" ) || name.starts_with( ".voltbl" ) || name.starts_with( ".gfids" ) ||
                 name.starts_with( ".giats" ) || name.starts_with( ".gljmp" ) || name.starts_with( ".gehcont" ) ||
                 name.starts_with( ".gxfg" ) || name.starts_with( ".sxdata" ) )
            {
                return Kind::Discard;
            }
            if ( name == ".tls" || name.starts_with( ".tls$" ) )
            {
                return Kind::Tls;
            }
            if ( name.starts_with( ".CRT$XC" ) )
            {
                return Kind::CrtInit;
            }
            if ( name.starts_with( ".CRT$" ) )
            {
                return Kind::Ignored;
            }
            if ( name == ".pdata" )
            {
                return Kind::PData;
            }
            if ( ( section.Characteristics & ( kScnCode | kScnMemExecute ) ) != 0 )
            {
                return Kind::Code;
            }
            if ( ( section.Characteristics & kScnMemWrite ) != 0 )
            {
                return Kind::Data;
            }
            return Kind::ReadOnly;
        }

        Region RegionOf( Kind kind )
        {
            switch ( kind )
            {
                case Kind::Code:
                    return Region::Code;
                case Kind::PData:
                    return Region::PData;
                case Kind::Data:
                    return Region::Data;
                default:
                    return Region::ReadOnly;
            }
        }

        std::uint32_t AlignmentOf( const Coff::Section& section )
        {
            const std::uint32_t log = ( section.Characteristics & kScnAlignMask ) >> 20;
            return log == 0 ? 16u : 1u << ( log - 1 );
        }

        std::uintptr_t AlignUp( std::uintptr_t value, std::uintptr_t alignment )
        {
            return ( value + alignment - 1 ) & ~( alignment - 1 );
        }

        /// A symbol only its own section refers to: labels ($LN, $unwind$, $pdata$), section symbols, aux slots.
        bool IsLocal( const Coff::Symbol& symbol )
        {
            return symbol.Name.empty() || symbol.Name[0] == '$' || symbol.Name[0] == '.' ||
                   symbol.StorageClass == kClassLabel;
        }

        std::size_t SizeOfImage( std::uintptr_t base )
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>( base );
            const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>( base + dos->e_lfanew );
            return nt->OptionalHeader.SizeOfImage;
        }

        void* ReserveBetween( std::uintptr_t from, std::uintptr_t to, std::size_t size )
        {
            std::uintptr_t at = from;
            while ( at < to )
            {
                MEMORY_BASIC_INFORMATION info{};
                if ( VirtualQuery( reinterpret_cast<void*>( at ), &info, sizeof( info ) ) == 0 )
                {
                    return nullptr;
                }
                const std::uintptr_t regionBegin = reinterpret_cast<std::uintptr_t>( info.BaseAddress );
                const std::uintptr_t regionEnd   = regionBegin + info.RegionSize;
                if ( info.State == MEM_FREE )
                {
                    const std::uintptr_t start = AlignUp( std::max( at, regionBegin ), 0x10000 );
                    if ( start + size <= regionEnd && start + size <= to )
                    {
                        if ( void* memory = VirtualAlloc( reinterpret_cast<void*>( start ), size,
                                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE ) )
                        {
                            return memory;
                        }
                    }
                }
                at = regionEnd;
            }
            return nullptr;
        }

        /// A section symbol (".data", ".bss"): a reference through it names an offset in the section, not a
        /// variable.
        bool IsSectionSymbol( const Coff::Symbol& symbol )
        {
            return symbol.StorageClass == kClassStatic && !symbol.Name.empty() && symbol.Name[0] == '.';
        }

        /// One object per type, which objects and the runtime compare by address: a vtable and its complete
        /// object locator (vtable[-1], ADDR64). The other RTTI records (??_R0..??_R3) are compared by name and
        /// reached image-relative, so the patch keeps its own copy of them.
        bool IsIdentityConstant( const std::string& name )
        {
            return name.starts_with( "??_7" ) || name.starts_with( "??_R4" );
        }

        /// The addend the compiler stored at the relocation's place.
        std::int64_t StoredAddend( const Coff::Section& section, const Coff::Relocation& relocation )
        {
            if ( relocation.Kind == Coff::RelocationKind::Addr64 )
            {
                std::int64_t addend = 0;
                if ( std::size_t( relocation.Offset ) + 8 <= section.Bytes.size() )
                {
                    std::memcpy( &addend, section.Bytes.data() + relocation.Offset, 8 );
                }
                return addend;
            }
            std::int32_t addend = 0;
            if ( std::size_t( relocation.Offset ) + 4 <= section.Bytes.size() )
            {
                std::memcpy( &addend, section.Bytes.data() + relocation.Offset, 4 );
            }
            return addend;
        }

        /// A laid-out unit of a section: the whole section, or, for writable data outside any COMDAT (no /Gw:
        /// every variable of a source in one .data/.bss), the bytes of one variable. A piece is laid out only when
        /// the patch needs it, so a variable the image has is never copied beside a new one of the same section.
        struct Piece
        {
            std::uint32_t              Begin    = 0;
            std::uint32_t              End      = 0;
            std::uint32_t              Owner    = kNoSymbol; // the named symbol at Begin, in a split section
            bool                       Included = false;
            std::uintptr_t             Address  = 0;
            std::vector<std::uint32_t> Relocations; // indices into the section's relocations inside [Begin, End)
        };

        /// The objects of one patch being linked: every decision, every placement, the first refusal.
        class Linker
        {
        public:
            Linker( std::span<const Coff::Object> objects, const ImageSymbols& image,
                    const std::unordered_set<std::string>& carrying, PatchReport& report )
                 : m_Objects( objects ), m_Image( image ), m_Report( report )
            {
                for ( const std::string& name : carrying )
                {
                    m_Carrying.insert( NormaliseAnonymousNamespace( name ) );
                }
            }

            Common::BoolResultStr Link();

            std::uintptr_t            Begin = 0;
            std::uintptr_t            End   = 0;
            std::vector<Redirect>     Redirects;
            std::vector<void ( * )()> Initialisers;
            PatchHistory              Defined;

        private:
            bool Fail( std::string reason )
            {
                if ( m_Error.empty() )
                {
                    m_Error = std::move( reason );
                }
                return false;
            }

            std::string_view MapObject( std::uint32_t object ) const
            {
                return object < m_Image.MapObjects.size() ? std::string_view( m_Image.MapObjects[object] )
                                                          : std::string_view();
            }

            std::optional<std::uint32_t> MapRva( std::uint32_t object, const std::string& name ) const
            {
                return m_Image.Map->Find( name, MapObject( object ) );
            }

            const OriginalObject* Original( std::uint32_t object ) const
            {
                return object < m_Image.Originals.size() ? m_Image.Originals[object] : nullptr;
            }

            /// The newest definition an applied patch of this image made of @p name.
            const PatchHistory::Definition* Earlier( const std::string& name ) const
            {
                if ( m_Image.History == nullptr )
                {
                    return nullptr;
                }
                const auto it = m_Image.History->Latest.find( NormaliseAnonymousNamespace( name ) );
                return it == m_Image.History->Latest.end() ? nullptr : &it->second;
            }

            std::uintptr_t Export( const std::string& name ) const
            {
                if ( auto* address = GetProcAddress( reinterpret_cast<HMODULE>( m_Image.Base ), name.c_str() ) )
                {
                    return reinterpret_cast<std::uintptr_t>( address );
                }
                for ( const std::uintptr_t base : m_Image.OtherImages )
                {
                    if ( auto* address = GetProcAddress( reinterpret_cast<HMODULE>( base ), name.c_str() ) )
                    {
                        return reinterpret_cast<std::uintptr_t>( address );
                    }
                }
                return 0;
            }

            bool IsChosen( std::uint32_t object, std::uint32_t symbol ) const
            {
                const Coff::Symbol& s = m_Objects[object].Symbols()[symbol];
                if ( s.StorageClass != kClassExternal )
                {
                    return true;
                }
                const auto it = m_Externals.find( s.Name );
                return it == m_Externals.end() || it->second == std::make_pair( object, symbol );
            }

            std::uint32_t PieceOf( std::uint32_t object, std::uint32_t section, std::uint32_t offset ) const
            {
                const std::vector<Piece>& pieces = m_Pieces[object][section];
                const auto                after =
                     std::upper_bound( pieces.begin(), pieces.end(), offset,
                                       []( std::uint32_t at, const Piece& piece ) { return at < piece.Begin; } );
                return after == pieces.begin() ? 0u : std::uint32_t( after - pieces.begin() - 1 );
            }

            /// Where @p offset of the section is in the patch (its piece is laid out).
            std::uintptr_t AddressOf( std::uint32_t object, std::uint32_t section, std::uint32_t offset ) const
            {
                const Piece& piece = m_Pieces[object][section][PieceOf( object, section, offset )];
                return piece.Address + ( offset - piece.Begin );
            }

            /// The symbol's own piece is laid out.
            bool IsIncluded( std::uint32_t object, std::uint32_t symbol ) const
            {
                const Coff::Symbol& s       = m_Objects[object].Symbols()[symbol];
                const std::uint32_t section = std::uint32_t( s.Section ) - 1;
                return m_Pieces[object][section][PieceOf( object, section, s.Value )].Included;
            }

            /// The offset in its section a reference through @p symbol with @p rest past it lands at.
            std::uint32_t OffsetOf( const Coff::Symbol& symbol, std::int64_t rest ) const
            {
                return IsSectionSymbol( symbol ) ? std::uint32_t( std::int64_t( symbol.Value ) + rest )
                                                 : symbol.Value;
            }

            bool           Decide( std::uint32_t object, std::uint32_t symbol, Decision& out );
            bool           Resolve( std::uint32_t object, const Coff::Relocation& relocation, std::int64_t addend,
                                    Decision& out, std::int64_t& rest );
            bool           CheckDataSize( std::uint32_t object, std::uint32_t symbol );
            void           Include( std::uint32_t object, std::uint32_t section, std::uint32_t offset );
            bool           Process( std::uint32_t object, std::uint32_t section, std::uint32_t piece );
            bool           Drain();
            bool           ReadInitialisers();
            bool           SameAsOriginal( std::uint32_t object, std::uint32_t symbol ) const;
            bool           IncludePData();
            bool           CheckThrowInfo();
            void           SplitSections( std::uint32_t object );
            bool           Layout();
            bool           Relocate( std::uint32_t object, std::uint32_t section, const Piece& piece );
            std::uintptr_t Thunk( std::uintptr_t target );
            std::uintptr_t TargetOf( const Decision& decision, bool imageRelative, std::int64_t rest ) const;
            bool           Report();

            std::span<const Coff::Object>   m_Objects;
            const ImageSymbols&             m_Image;
            PatchReport&                    m_Report;
            std::unordered_set<std::string> m_Carrying; // normalised
            std::string                     m_Error;

            std::vector<std::vector<Kind>>                       m_Kinds;
            std::vector<std::vector<Decision>>                   m_Decisions;
            std::vector<std::vector<bool>>                       m_Included;   // some piece of the section is
            std::vector<std::vector<std::vector<Piece>>>         m_Pieces;     // object -> section -> pieces
            std::vector<std::vector<std::vector<std::uint32_t>>> m_Associates; // parent section -> associatives
            std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>> m_Externals;
            std::set<std::pair<std::uint32_t, std::uint32_t>>    m_LocalCopies; // image data copied for ADDR32NB
            std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> m_Work; // object, section, piece
            std::vector<std::pair<std::uint32_t, std::uint32_t>>                 m_InitialiserSymbols;

            std::set<std::uintptr_t> m_ThunkTargets; // upper bound, counted while including
            std::set<std::uintptr_t> m_SlotTargets;
            std::unordered_map<std::uintptr_t, std::uintptr_t> m_Thunks;
            std::unordered_map<std::uintptr_t, std::uintptr_t> m_Slots;
            std::uintptr_t                                     m_NextThunk = 0;
            std::uintptr_t                                     m_ThunkEnd  = 0;
            std::uintptr_t m_RegionBegin[std::size_t( Region::Count ) + 1] = {};
            std::uintptr_t m_PDataEnd                                      = 0;
        };

        bool Linker::Decide( std::uint32_t object, std::uint32_t symbol, Decision& out )
        {
            const std::span<const Coff::Symbol> symbols = m_Objects[object].Symbols();
            if ( symbol >= symbols.size() )
            {
                return Fail( m_Objects[object].Path().generic_string() + ": a relocation names no symbol" );
            }
            Decision& memo = m_Decisions[object][symbol];
            if ( memo.Place != Where::Undecided )
            {
                out = memo;
                return true;
            }
            const Coff::Symbol& s = symbols[symbol];
            Decision            decision;
            decision.Function = s.Function;

            const bool external = s.StorageClass == kClassExternal || s.StorageClass == kClassWeakExternal;
            if ( external )
            {
                const auto chosen = m_Externals.find( s.Name );
                if ( chosen != m_Externals.end() && chosen->second != std::make_pair( object, symbol ) )
                {
                    if ( !Decide( chosen->second.first, chosen->second.second, decision ) )
                    {
                        return false;
                    }
                    memo = decision;
                    out  = decision;
                    return true;
                }
            }

            if ( s.Section > 0 )
            {
                const std::uint32_t section = std::uint32_t( s.Section ) - 1;
                decision.DefObject          = object;
                decision.DefSymbol          = symbol;
                switch ( m_Kinds[object][section] )
                {
                    case Kind::Discard:
                    case Kind::CrtInit:
                    case Kind::Ignored:
                        return Fail( s.Name + " is referenced in " + m_Objects[object].Sections()[section].Name +
                                     ", a section a patch does not load" );
                    case Kind::Tls:
                    {
                        const auto rva = MapRva( object, s.Name );
                        if ( !rva )
                        {
                            return Fail( "new thread_local " + s.Name );
                        }
                        decision.Place   = Where::Image;
                        decision.Address = m_Image.Base + *rva;
                        break;
                    }
                    case Kind::Code:
                        decision.Place = Where::Patch;
                        if ( !IsLocal( s ) && m_Carrying.count( NormaliseAnonymousNamespace( s.Name ) ) == 0 )
                        {
                            // Carries no edited code: the copy that runs stays, the image's or an earlier patch's.
                            if ( const auto rva = MapRva( object, s.Name ) )
                            {
                                decision.Place   = Where::Image;
                                decision.Address = m_Image.Base + *rva;
                            }
                            else if ( const PatchHistory::Definition* earlier = Earlier( s.Name ) )
                            {
                                decision.Place   = Where::Image;
                                decision.Address = earlier->Address;
                            }
                        }
                        break;
                    case Kind::ReadOnly:
                        decision.Place = Where::Patch; // an edited constant: the new value
                        if ( !IsLocal( s ) )
                        {
                            // A vtable or an RTTI record stays the one objects already point at; an unchanged
                            // constant stays the image's (throw information among them, see CheckThrowInfo).
                            const bool identity = IsIdentityConstant( s.Name );
                            const auto rva      = MapRva( object, s.Name );
                            if ( rva && ( identity || SameAsOriginal( object, symbol ) ) )
                            {
                                decision.Place   = Where::Image;
                                decision.Address = m_Image.Base + *rva;
                            }
                            else if ( const PatchHistory::Definition* earlier = Earlier( s.Name );
                                      !rva && identity && earlier != nullptr )
                            {
                                decision.Place   = Where::Image;
                                decision.Address = earlier->Address;
                            }
                        }
                        break;
                    case Kind::PData:
                        decision.Place = Where::Patch;
                        break;
                    case Kind::Data:
                        decision.Place = Where::Patch;
                        if ( !IsLocal( s ) )
                        {
                            // Live data stays where it is: in the image, or where the patch that added it put it.
                            if ( const auto rva = MapRva( object, s.Name ) )
                            {
                                if ( !CheckDataSize( object, symbol ) )
                                {
                                    return false;
                                }
                                decision.Place   = Where::Image;
                                decision.Address = m_Image.Base + *rva;
                            }
                            else if ( const PatchHistory::Definition* earlier = Earlier( s.Name ) )
                            {
                                const std::uint32_t now = m_Objects[object].Extent( symbol );
                                if ( now > earlier->Size )
                                {
                                    return Fail( "data layout changed: " + s.Name + " (" +
                                                 std::to_string( earlier->Size ) + " -> " + std::to_string( now ) +
                                                 " bytes)" );
                                }
                                decision.Place   = Where::Image;
                                decision.Address = earlier->Address;
                            }
                        }
                        break;
                }
            }
            else if ( s.Section == -1 )
            {
                decision.Place   = Where::Image;
                decision.Address = s.Value;
            }
            else if ( s.Section == 0 && external )
            {
                if ( s.Name == "__ImageBase" )
                {
                    decision.Place   = Where::Image;
                    decision.Address = m_Image.Base;
                }
                else if ( const auto rva = m_Image.Map->Find( s.Name, {} ) )
                {
                    decision.Place   = Where::Image; // includes the image's import thunks and IAT slots
                    decision.Address = m_Image.Base + *rva;
                }
                else if ( s.Name.starts_with( "__imp_" ) )
                {
                    const std::uintptr_t address = Export( s.Name.substr( 6 ) );
                    if ( address == 0 )
                    {
                        return Fail( "unresolved import " + s.Name.substr( 6 ) +
                                     ": no loaded image of the build exports it" );
                    }
                    decision.Place   = Where::Slot;
                    decision.Address = address;
                }
                else if ( const std::uintptr_t address = Export( s.Name ) )
                {
                    decision.Place   = Where::Image;
                    decision.Address = address;
                }
                else if ( s.StorageClass == kClassWeakExternal && s.WeakDefault != kNoSymbol )
                {
                    // Nothing defines the weak name: it is its default (the aux record's TagIndex), as link.exe
                    // resolves it.
                    if ( !Decide( object, s.WeakDefault, decision ) )
                    {
                        return false;
                    }
                }
                else
                {
                    return Fail( "unresolved symbol " + s.Name + ": not in the image's map nor exported" );
                }
            }
            else
            {
                return Fail( "symbol " + s.Name + " of " + m_Objects[object].Path().generic_string() +
                             " has no definition a patch can use" );
            }
            memo = decision;
            out  = decision;
            return true;
        }

        // The decision a relocation lands on and the bytes past that decision's symbol. A reference through the
        // section symbol of a split data section lands on the variable at its offset: the image's when the image
        // has that variable.
        bool Linker::Resolve( std::uint32_t object, const Coff::Relocation& relocation, std::int64_t addend,
                              Decision& out, std::int64_t& rest )
        {
            if ( !Decide( object, relocation.Symbol, out ) )
            {
                return false;
            }
            rest = addend;
            if ( out.Place != Where::Patch )
            {
                return true;
            }
            const Coff::Symbol& s = m_Objects[out.DefObject].Symbols()[out.DefSymbol];
            if ( !IsSectionSymbol( s ) )
            {
                return true;
            }
            const std::uint32_t section = std::uint32_t( s.Section ) - 1;
            const std::int64_t  offset  = std::int64_t( s.Value ) + addend;
            if ( offset < 0 || offset > std::int64_t( m_Objects[out.DefObject].Sections()[section].Size ) )
            {
                return Fail( s.Name + " of " + m_Objects[out.DefObject].Path().generic_string() +
                             " is referenced outside the section" );
            }
            const Piece& piece =
                 m_Pieces[out.DefObject][section][PieceOf( out.DefObject, section, std::uint32_t( offset ) )];
            if ( piece.Owner == kNoSymbol )
            {
                return true;
            }
            Decision owner;
            if ( !Decide( out.DefObject, piece.Owner, owner ) )
            {
                return false;
            }
            if ( owner.Place != Where::Patch )
            {
                out  = owner;
                rest = offset - std::int64_t( piece.Begin );
            }
            return true;
        }

        bool Linker::CheckDataSize( std::uint32_t object, std::uint32_t symbol )
        {
            const OriginalObject* original = Original( object );
            if ( original == nullptr )
            {
                return true; // compiled alone before too and not reloaded: nothing to compare against
            }
            const Coff::Symbol&                s     = m_Objects[object].Symbols()[symbol];
            const std::optional<std::uint32_t> found = original->Find( NormaliseAnonymousNamespace( s.Name ) );
            if ( !found || original->Object().Symbols()[*found].Function )
            {
                return true; // defined by another object of the image: its size is not this object's to judge
            }
            const std::uint32_t before = original->Object().Extent( *found );
            const std::uint32_t after  = m_Objects[object].Extent( symbol );
            if ( after > before )
            {
                return Fail( "data layout changed: " + s.Name + " (" + std::to_string( before ) + " -> " +
                             std::to_string( after ) + " bytes)" );
            }
            return true;
        }

        void Linker::Include( std::uint32_t object, std::uint32_t section, std::uint32_t offset )
        {
            const Kind kind = m_Kinds[object][section];
            if ( kind == Kind::Discard || kind == Kind::Tls || kind == Kind::CrtInit || kind == Kind::Ignored )
            {
                return;
            }
            const std::uint32_t piece = PieceOf( object, section, offset );
            Piece&              unit  = m_Pieces[object][section][piece];
            if ( unit.Included )
            {
                return;
            }
            unit.Included               = true;
            m_Included[object][section] = true;
            m_Work.emplace_back( object, section, piece );
            for ( const std::uint32_t child : m_Associates[object][section] )
            {
                Include( object, child, 0 );
            }
        }

        bool Linker::Process( std::uint32_t object, std::uint32_t section, std::uint32_t piece )
        {
            const Coff::Section& s = m_Objects[object].Sections()[section];
            for ( const std::uint32_t index : m_Pieces[object][section][piece].Relocations )
            {
                const Coff::Relocation& relocation = s.Relocations[index];
                Decision                decision;
                std::int64_t            rest = 0;
                if ( !Resolve( object, relocation, StoredAddend( s, relocation ), decision, rest ) )
                {
                    return false;
                }
                const bool imageRelative = relocation.Kind == Coff::RelocationKind::Addr32Nb;
                const bool rel32         = relocation.Kind >= Coff::RelocationKind::Rel32 &&
                                   relocation.Kind <= Coff::RelocationKind::Rel32_5;
                const Kind targetKind =
                     decision.DefSymbol == kNoSymbol
                          ? Kind::Code
                          : m_Kinds[decision.DefObject]
                                   [std::uint32_t(
                                         m_Objects[decision.DefObject].Symbols()[decision.DefSymbol].Section ) -
                                    1];
                if ( decision.Place == Where::Image && imageRelative &&
                     ( targetKind == Kind::Data || targetKind == Kind::ReadOnly ) )
                {
                    // Image-relative from the patch's base (EH tables -> TypeDescriptor, ??_R2 -> ??_R1 of a
                    // base class): the image is out of reach, so the patch keeps its own copy.
                    m_LocalCopies.emplace( decision.DefObject, decision.DefSymbol );
                    decision.Place = Where::Patch;
                }
                if ( decision.Place == Where::Patch )
                {
                    const Coff::Symbol& target = m_Objects[decision.DefObject].Symbols()[decision.DefSymbol];
                    Include( decision.DefObject, std::uint32_t( target.Section ) - 1, OffsetOf( target, rest ) );
                }
                else if ( decision.Place == Where::Slot )
                {
                    m_SlotTargets.insert( decision.Address );
                }
                else if ( imageRelative || rel32 )
                {
                    m_ThunkTargets.insert( decision.Address + std::uintptr_t( rest ) );
                }
            }
            return true;
        }

        bool Linker::Drain()
        {
            while ( !m_Work.empty() )
            {
                const auto [object, section, piece] = m_Work.back();
                m_Work.pop_back();
                if ( !Process( object, section, piece ) )
                {
                    return false;
                }
            }
            return true;
        }

        // The symbol's bytes and relocations (place, kind, target name) equal those of the same-named symbol of
        // the object the image was linked from. A COMDAT is its whole section; other symbols are their extent.
        bool Linker::SameAsOriginal( std::uint32_t object, std::uint32_t symbol ) const
        {
            const OriginalObject* original = Original( object );
            if ( original == nullptr )
            {
                return false;
            }
            const Coff::Object&                now = m_Objects[object];
            const std::optional<std::uint32_t> found =
                 original->Find( NormaliseAnonymousNamespace( now.Symbols()[symbol].Name ) );
            if ( !found )
            {
                return false;
            }
            struct Range
            {
                const Coff::Object*  Object;
                const Coff::Section* Section;
                std::uint32_t        Begin;
                std::uint32_t        Size;
            };
            const auto rangeOf = []( const Coff::Object& o, std::uint32_t index ) -> std::optional<Range>
            {
                const Coff::Symbol& s = o.Symbols()[index];
                if ( s.Section <= 0 || std::size_t( s.Section ) > o.Sections().size() )
                {
                    return std::nullopt;
                }
                const Coff::Section& section = o.Sections()[std::size_t( s.Section ) - 1];
                if ( section.Comdat != Coff::ComdatSelect::None )
                {
                    return Range{ &o, &section, 0, section.Size };
                }
                return Range{ &o, &section, s.Value, o.Extent( index ) };
            };
            const std::optional<Range> before = rangeOf( original->Object(), *found );
            const std::optional<Range> after  = rangeOf( now, symbol );
            if ( !before || !after || before->Size != after->Size ||
                 before->Section->Bytes.empty() != after->Section->Bytes.empty() )
            {
                return false;
            }
            if ( !before->Section->Bytes.empty() &&
                 ( std::size_t( before->Begin ) + before->Size > before->Section->Bytes.size() ||
                   std::size_t( after->Begin ) + after->Size > after->Section->Bytes.size() ||
                   std::memcmp( before->Section->Bytes.data() + before->Begin,
                                after->Section->Bytes.data() + after->Begin, before->Size ) != 0 ) )
            {
                return false;
            }
            using Reference       = std::tuple<std::uint32_t, Coff::RelocationKind, std::string>;
            const auto references = []( const Range& range )
            {
                std::vector<Reference> list;
                for ( const Coff::Relocation& r : range.Section->Relocations )
                {
                    if ( r.Offset >= range.Begin && r.Offset < range.Begin + range.Size &&
                         r.Symbol < range.Object->Symbols().size() )
                    {
                        list.emplace_back( r.Offset - range.Begin, r.Kind,
                                           NormaliseAnonymousNamespace( range.Object->Symbols()[r.Symbol].Name ) );
                    }
                }
                std::sort( list.begin(), list.end() );
                return list;
            };
            return references( *before ) == references( *after );
        }

        // The .CRT$XCU entries: one `??__E<variable>` per dynamically initialised variable. The initialiser of a
        // variable the image and the earlier patches lack runs before the redirects; one of a variable that exists
        // is never rerun (it would reinitialise live data), and is reported when its code changed.
        bool Linker::ReadInitialisers()
        {
            for ( std::uint32_t object = 0; object < m_Objects.size(); ++object )
            {
                const std::span<const Coff::Symbol>  symbols  = m_Objects[object].Symbols();
                const std::span<const Coff::Section> sections = m_Objects[object].Sections();
                for ( std::uint32_t section = 0; section < sections.size(); ++section )
                {
                    if ( m_Kinds[object][section] != Kind::CrtInit )
                    {
                        continue;
                    }
                    for ( const Coff::Relocation& relocation : sections[section].Relocations )
                    {
                        if ( relocation.Kind != Coff::RelocationKind::Addr64 ||
                             relocation.Symbol >= symbols.size() || symbols[relocation.Symbol].Section <= 0 )
                        {
                            return Fail( "an initialiser table entry of " +
                                         m_Objects[object].Path().generic_string() +
                                         " names no function of the object" );
                        }
                        const std::uint32_t initialiser = relocation.Symbol;
                        const std::string&  name        = symbols[initialiser].Name;

                        // "??__Eg@VFX@Desert@@YAXXZ" -> "?g@VFX@Desert@@...", "??__E?s@Foo@@2HA@@YAXXZ" ->
                        // "?s@Foo@@2HA"
                        std::string variable;
                        bool        exact = false;
                        if ( name.starts_with( "??__E" ) && name.ends_with( "YAXXZ" ) )
                        {
                            std::string rest = name.substr( 5, name.size() - 10 );
                            if ( rest.starts_with( "?" ) && rest.ends_with( "@@" ) )
                            {
                                variable = rest.substr( 0, rest.size() - 2 );
                                exact    = true;
                            }
                            else
                            {
                                variable = "?" + rest;
                            }
                        }
                        std::uint32_t found = kNoSymbol;
                        for ( std::uint32_t i = 0; !variable.empty() && i < symbols.size(); ++i )
                        {
                            const Coff::Symbol& s = symbols[i];
                            if ( s.Section > 0 && !s.Function && !IsLocal( s ) &&
                                 ( exact ? s.Name == variable : s.Name.starts_with( variable ) ) )
                            {
                                found = i;
                                break;
                            }
                        }

                        Decision decision;
                        if ( found != kNoSymbol && !Decide( object, found, decision ) )
                        {
                            return false;
                        }
                        if ( found != kNoSymbol && decision.Place == Where::Patch )
                        {
                            m_InitialiserSymbols.emplace_back( object, initialiser );
                            Include( object, std::uint32_t( symbols[initialiser].Section ) - 1,
                                     symbols[initialiser].Value );
                        }
                        else if ( !SameAsOriginal( object, initialiser ) )
                        {
                            m_Report.NotRerun.push_back( found != kNoSymbol ? symbols[found].Name : name );
                        }
                    }
                }
            }
            return true;
        }

        // A .pdata outside any COMDAT (code compiled without /Gy) covers several functions: it is taken whole when
        // every function it covers is in the patch, and refused when only some are.
        bool Linker::IncludePData()
        {
            for ( std::uint32_t object = 0; object < m_Objects.size(); ++object )
            {
                const std::span<const Coff::Section> sections = m_Objects[object].Sections();
                for ( std::uint32_t section = 0; section < sections.size(); ++section )
                {
                    if ( m_Kinds[object][section] != Kind::PData || m_Included[object][section] ||
                         sections[section].Comdat != Coff::ComdatSelect::None )
                    {
                        continue;
                    }
                    std::size_t covered = 0, total = 0;
                    for ( const Coff::Relocation& relocation : sections[section].Relocations )
                    {
                        if ( relocation.Offset % 12 != 0 )
                        {
                            continue; // the BeginAddress field of each entry
                        }
                        ++total;
                        const Coff::Symbol& target = m_Objects[object].Symbols()[relocation.Symbol];
                        if ( target.Section > 0 && m_Included[object][std::uint32_t( target.Section ) - 1] )
                        {
                            ++covered;
                        }
                    }
                    if ( covered != 0 && covered != total )
                    {
                        return Fail( m_Objects[object].Path().generic_string() +
                                     ": one .pdata covers patched and unpatched functions (compile with /Gy)" );
                    }
                    if ( covered != 0 )
                    {
                        Include( object, section, 0 );
                    }
                }
            }
            return Drain();
        }

        // A throw hands its _ThrowInfo to the runtime, which finds the image the information lives in with
        // RtlPcToFileHeader; patch memory is no image, so a throw whose information only the patch has would
        // terminate the editor. Unchanged throw information resolves to the image's (Decide, ReadOnly).
        bool Linker::CheckThrowInfo()
        {
            for ( std::uint32_t object = 0; object < m_Objects.size(); ++object )
            {
                const std::span<const Coff::Symbol> symbols = m_Objects[object].Symbols();
                for ( std::uint32_t i = 0; i < symbols.size(); ++i )
                {
                    const Coff::Symbol& s = symbols[i];
                    if ( s.Section <= 0 || !( s.Name.starts_with( "_TI" ) || s.Name.starts_with( "_CTA" ) ) ||
                         !IsChosen( object, i ) || !IsIncluded( object, i ) )
                    {
                        continue;
                    }
                    return Fail( "the patch adds exception type information the image lacks (" + s.Name +
                                 "): the C++ runtime reads it only from a registered image; rebuild and restart" );
                }
            }
            return true;
        }

        void Linker::SplitSections( std::uint32_t object )
        {
            const std::span<const Coff::Section>    sections = m_Objects[object].Sections();
            const std::span<const Coff::Symbol>     symbols  = m_Objects[object].Symbols();
            std::vector<std::vector<std::uint32_t>> bySection( sections.size() );
            for ( std::uint32_t i = 0; i < symbols.size(); ++i )
            {
                const Coff::Symbol& s = symbols[i];
                if ( s.Section > 0 && std::size_t( s.Section ) <= sections.size() && !IsSectionSymbol( s ) )
                {
                    bySection[std::size_t( s.Section ) - 1].push_back( i );
                }
            }
            m_Pieces[object].resize( sections.size() );
            for ( std::uint32_t section = 0; section < sections.size(); ++section )
            {
                const Coff::Section&       s     = sections[section];
                std::vector<Piece>&        units = m_Pieces[object][section];
                std::vector<std::uint32_t> bounds{ 0 };
                const bool split = m_Kinds[object][section] == Kind::Data && s.Comdat == Coff::ComdatSelect::None;
                if ( split )
                {
                    for ( const std::uint32_t i : bySection[section] )
                    {
                        if ( symbols[i].Value < s.Size )
                        {
                            bounds.push_back( symbols[i].Value );
                        }
                    }
                    std::sort( bounds.begin(), bounds.end() );
                    bounds.erase( std::unique( bounds.begin(), bounds.end() ), bounds.end() );
                }
                for ( std::size_t k = 0; k < bounds.size(); ++k )
                {
                    Piece piece;
                    piece.Begin = bounds[k];
                    piece.End   = k + 1 < bounds.size() ? bounds[k + 1] : s.Size;
                    units.push_back( std::move( piece ) );
                }
                if ( split )
                {
                    for ( const std::uint32_t i : bySection[section] )
                    {
                        Piece& piece = units[PieceOf( object, section, symbols[i].Value )];
                        if ( piece.Begin == symbols[i].Value &&
                             ( piece.Owner == kNoSymbol ||
                               ( IsLocal( symbols[piece.Owner] ) && !IsLocal( symbols[i] ) ) ) )
                        {
                            piece.Owner = i;
                        }
                    }
                }
                for ( std::uint32_t r = 0; r < s.Relocations.size(); ++r )
                {
                    units[PieceOf( object, section, s.Relocations[r].Offset )].Relocations.push_back( r );
                }
            }
        }

        bool Linker::Layout()
        {
            std::uintptr_t                                        size[std::size_t( Region::Count )] = {};
            std::vector<std::vector<std::vector<std::uintptr_t>>> offsets( m_Objects.size() );
            for ( std::uint32_t object = 0; object < m_Objects.size(); ++object )
            {
                const std::span<const Coff::Section> sections = m_Objects[object].Sections();
                offsets[object].resize( sections.size() );
                for ( std::uint32_t section = 0; section < sections.size(); ++section )
                {
                    if ( !m_Included[object][section] )
                    {
                        continue;
                    }
                    const Kind          kind      = m_Kinds[object][section];
                    std::uintptr_t&     end       = size[std::size_t( RegionOf( kind ) )];
                    const std::uint32_t alignment = kind == Kind::PData ? 4u : AlignmentOf( sections[section] );
                    offsets[object][section].assign( m_Pieces[object][section].size(), 0 );
                    for ( std::size_t p = 0; p < m_Pieces[object][section].size(); ++p )
                    {
                        const Piece& piece = m_Pieces[object][section][p];
                        if ( !piece.Included )
                        {
                            continue;
                        }
                        // Code: the hotpatch padding a later patch redirects this function through.
                        end = AlignUp( end + ( kind == Kind::Code ? kCodePadding : 0 ), alignment );
                        offsets[object][section][p] = end;
                        end += piece.End - piece.Begin;
                    }
                }
            }
            std::uintptr_t&      code        = size[std::size_t( Region::Code )];
            const std::uintptr_t thunkOffset = AlignUp( code, kThunkSize );
            code                             = thunkOffset + m_ThunkTargets.size() * kThunkSize;
            std::uintptr_t&      readOnly    = size[std::size_t( Region::ReadOnly )];
            const std::uintptr_t slotOffset  = AlignUp( readOnly, 8 );
            readOnly                         = slotOffset + m_SlotTargets.size() * 8;

            std::uintptr_t regionOffset[std::size_t( Region::Count ) + 1] = {};
            for ( std::size_t r = 0; r < std::size_t( Region::Count ); ++r )
            {
                regionOffset[r + 1] = regionOffset[r] + AlignUp( std::max<std::uintptr_t>( size[r], 1 ), 0x1000 );
            }
            const std::size_t total = regionOffset[std::size_t( Region::Count )];

            const std::size_t    imageSize = SizeOfImage( m_Image.Base );
            const std::uintptr_t imageEnd  = m_Image.Base + imageSize;
            const std::uintptr_t lowest    = imageEnd > kReach + 0x10000 ? imageEnd - kReach : 0x10000;
            const std::uintptr_t highest   = m_Image.Base + kReach;
            void*                memory    = ReserveBetween( imageEnd, highest, total );
            if ( memory == nullptr )
            {
                memory = ReserveBetween( lowest, m_Image.Base, total );
            }
            if ( memory == nullptr )
            {
                return Fail( "no free address range of " + std::to_string( total ) +
                             " bytes within 2 GB of the image" );
            }
            Begin = reinterpret_cast<std::uintptr_t>( memory );
            End   = Begin + total;
            for ( std::size_t r = 0; r <= std::size_t( Region::Count ); ++r )
            {
                m_RegionBegin[r] = Begin + regionOffset[r];
            }
            m_PDataEnd = m_RegionBegin[std::size_t( Region::PData )] + size[std::size_t( Region::PData )];
            // int3 between functions: the padding before each entry, and whatever the alignment leaves.
            std::memset( reinterpret_cast<void*>( m_RegionBegin[std::size_t( Region::Code )] ), 0xCC,
                         m_RegionBegin[std::size_t( Region::Code ) + 1] -
                              m_RegionBegin[std::size_t( Region::Code )] );

            for ( std::uint32_t object = 0; object < m_Objects.size(); ++object )
            {
                const std::span<const Coff::Section> sections = m_Objects[object].Sections();
                for ( std::uint32_t section = 0; section < sections.size(); ++section )
                {
                    if ( !m_Included[object][section] )
                    {
                        continue;
                    }
                    const std::uintptr_t region =
                         m_RegionBegin[std::size_t( RegionOf( m_Kinds[object][section] ) )];
                    for ( std::size_t p = 0; p < m_Pieces[object][section].size(); ++p )
                    {
                        Piece& piece = m_Pieces[object][section][p];
                        if ( !piece.Included )
                        {
                            continue;
                        }
                        piece.Address = region + offsets[object][section][p];
                        // Uninitialised data stays the zeroes VirtualAlloc gave.
                        if ( !sections[section].Bytes.empty() && piece.End > piece.Begin )
                        {
                            std::memcpy( reinterpret_cast<void*>( piece.Address ),
                                         sections[section].Bytes.data() + piece.Begin, piece.End - piece.Begin );
                        }
                    }
                }
            }
            m_NextThunk         = m_RegionBegin[std::size_t( Region::Code )] + thunkOffset;
            m_ThunkEnd          = m_NextThunk + m_ThunkTargets.size() * kThunkSize;
            std::uintptr_t slot = m_RegionBegin[std::size_t( Region::ReadOnly )] + slotOffset;
            for ( const std::uintptr_t target : m_SlotTargets )
            {
                std::memcpy( reinterpret_cast<void*>( slot ), &target, sizeof( target ) );
                m_Slots.emplace( target, slot );
                slot += 8;
            }
            return true;
        }

        std::uintptr_t Linker::Thunk( std::uintptr_t target )
        {
            const auto known = m_Thunks.find( target );
            if ( known != m_Thunks.end() )
            {
                return known->second;
            }
            if ( m_NextThunk + kThunkSize > m_ThunkEnd )
            {
                return 0;
            }
            std::uint8_t code[kThunkSize] = { 0xFF, 0x25, 0, 0, 0, 0 }; // jmp qword ptr [rip+0]
            std::memcpy( code + 6, &target, sizeof( target ) );
            code[14] = code[15] = 0xCC;
            std::memcpy( reinterpret_cast<void*>( m_NextThunk ), code, kThunkSize );
            m_Thunks.emplace( target, m_NextThunk );
            m_NextThunk += kThunkSize;
            return m_NextThunk - kThunkSize;
        }

        // The address a reference lands at, @p rest (the addend) included.
        std::uintptr_t Linker::TargetOf( const Decision& decision, bool imageRelative, std::int64_t rest ) const
        {
            const bool localCopy = imageRelative && decision.DefSymbol != kNoSymbol &&
                                   m_LocalCopies.count( { decision.DefObject, decision.DefSymbol } ) != 0;
            if ( decision.Place == Where::Patch || localCopy )
            {
                const Coff::Symbol& s       = m_Objects[decision.DefObject].Symbols()[decision.DefSymbol];
                const std::uint32_t section = std::uint32_t( s.Section ) - 1;
                if ( IsSectionSymbol( s ) )
                {
                    return AddressOf( decision.DefObject, section, OffsetOf( s, rest ) );
                }
                return AddressOf( decision.DefObject, section, s.Value ) + std::uintptr_t( rest );
            }
            if ( decision.Place == Where::Slot )
            {
                return m_Slots.at( decision.Address ) + std::uintptr_t( rest );
            }
            return decision.Address + std::uintptr_t( rest );
        }

        bool Linker::Relocate( std::uint32_t object, std::uint32_t section, const Piece& piece )
        {
            const Coff::Section& s = m_Objects[object].Sections()[section];
            for ( const std::uint32_t index : piece.Relocations )
            {
                const Coff::Relocation& relocation = s.Relocations[index];
                const std::uintptr_t    at         = piece.Address + ( relocation.Offset - piece.Begin );
                const std::size_t       width      = relocation.Kind == Coff::RelocationKind::Addr64 ? 8 : 4;
                if ( std::size_t( relocation.Offset ) + width > piece.End )
                {
                    return Fail( s.Name + ": a relocation lies past the end of its section or variable" );
                }
                Decision     decision;
                std::int64_t rest = 0;
                if ( !Resolve( object, relocation, StoredAddend( s, relocation ), decision, rest ) )
                {
                    return false;
                }
                const std::string& name = m_Objects[object].Symbols()[relocation.Symbol].Name;
                switch ( relocation.Kind )
                {
                    case Coff::RelocationKind::Addr64:
                    {
                        const std::uint64_t value = TargetOf( decision, false, rest );
                        std::memcpy( reinterpret_cast<void*>( at ), &value, 8 );
                        break;
                    }
                    case Coff::RelocationKind::Addr32Nb:
                    {
                        std::uintptr_t target = TargetOf( decision, true, rest );
                        if ( target < Begin || target >= End )
                        {
                            if ( !decision.Function )
                            {
                                return Fail( "image-relative reference from the patch to data outside it: " +
                                             name );
                            }
                            // The EH handler (__CxxFrameHandler4) and other runtime functions.
                            target = Thunk( target );
                            if ( target == 0 )
                            {
                                return Fail( "the patch ran out of thunks resolving " + name );
                            }
                        }
                        const std::int64_t value = std::int64_t( target - Begin );
                        if ( value < 0 || value > std::int64_t( UINT32_MAX ) )
                        {
                            return Fail( "image-relative reference out of range: " + name );
                        }
                        const std::uint32_t written = std::uint32_t( value );
                        std::memcpy( reinterpret_cast<void*>( at ), &written, 4 );
                        break;
                    }
                    case Coff::RelocationKind::Rel32:
                    case Coff::RelocationKind::Rel32_1:
                    case Coff::RelocationKind::Rel32_2:
                    case Coff::RelocationKind::Rel32_3:
                    case Coff::RelocationKind::Rel32_4:
                    case Coff::RelocationKind::Rel32_5:
                    {
                        const std::int64_t trailing =
                             std::int64_t( relocation.Kind ) - std::int64_t( Coff::RelocationKind::Rel32 );
                        std::uintptr_t target = TargetOf( decision, false, rest );
                        std::int64_t   value  = std::int64_t( target ) - std::int64_t( at + 4 + trailing );
                        if ( value < INT32_MIN || value > INT32_MAX )
                        {
                            if ( !decision.Function )
                            {
                                return Fail( "the patch is more than 2 GB from the data " + name );
                            }
                            target = Thunk( target );
                            if ( target == 0 )
                            {
                                return Fail( "the patch ran out of thunks resolving " + name );
                            }
                            value = std::int64_t( target ) - std::int64_t( at + 4 + trailing );
                        }
                        const std::int32_t written = std::int32_t( value );
                        std::memcpy( reinterpret_cast<void*>( at ), &written, 4 );
                        break;
                    }
                    case Coff::RelocationKind::SecRel:
                    {
                        // thread_local access: the offset in the image's TLS template, which every thread copies.
                        const auto tls = m_Image.Map->SectionStart( ".tls" );
                        if ( decision.Place != Where::Image || !tls || decision.Address < m_Image.Base ||
                             decision.Address - m_Image.Base < *tls )
                        {
                            return Fail( "section-relative reference to " + name +
                                         ", which is no thread_local of the image" );
                        }
                        const std::uint32_t written =
                             std::uint32_t( decision.Address - m_Image.Base - *tls ) + std::uint32_t( rest );
                        std::memcpy( reinterpret_cast<void*>( at ), &written, 4 );
                        break;
                    }
                    default:
                        return Fail( "relocation type " + std::to_string( unsigned( relocation.Kind ) ) + " in " +
                                     s.Name + " of " + m_Objects[object].Path().generic_string() +
                                     " is not supported" );
                }
            }
            return true;
        }

        // The redirects (each function carrying edited code, from the image's entry and from every earlier
        // patch's entry), the report, and what this patch defines for the next one.
        bool Linker::Report()
        {
            for ( std::uint32_t object = 0; object < m_Objects.size(); ++object )
            {
                const std::span<const Coff::Symbol> symbols = m_Objects[object].Symbols();
                for ( std::uint32_t i = 0; i < symbols.size(); ++i )
                {
                    const Coff::Symbol& s = symbols[i];
                    if ( s.Section <= 0 || IsLocal( s ) || !IsChosen( object, i ) || !IsIncluded( object, i ) )
                    {
                        continue;
                    }
                    const Kind           kind    = m_Kinds[object][std::uint32_t( s.Section ) - 1];
                    const std::uintptr_t address = AddressOf( object, std::uint32_t( s.Section ) - 1, s.Value );
                    const std::string    key     = NormaliseAnonymousNamespace( s.Name );
                    if ( kind == Kind::Code && s.Function )
                    {
                        const auto rva = MapRva( object, s.Name );
                        if ( m_Carrying.count( key ) != 0 && rva )
                        {
                            if ( !HasHotpatchRoom( m_Image.Base + *rva ) )
                            {
                                return Fail( m_Image.Name + " was linked without /FUNCTIONPADMIN" );
                            }
                            Redirects.push_back( Redirect{ m_Image.Base + *rva, address, s.Name } );
                            m_Report.Redirected.push_back( s.Name );
                        }
                        else if ( !rva )
                        {
                            m_Report.NewFunctions.push_back( s.Name );
                        }
                        if ( m_Image.History != nullptr )
                        {
                            const auto earlier = m_Image.History->Entries.find( key );
                            if ( earlier != m_Image.History->Entries.end() )
                            {
                                for ( const std::uintptr_t entry : earlier->second )
                                {
                                    Redirects.push_back( Redirect{ entry, address, s.Name } );
                                }
                            }
                        }
                        Defined.Entries[key].push_back( address );
                        Defined.Latest[key] = PatchHistory::Definition{ address, 0, true };
                    }
                    else if ( kind == Kind::Data && m_LocalCopies.count( { object, i } ) == 0 )
                    {
                        m_Report.NewData.push_back( s.Name );
                        Defined.Latest[key] =
                             PatchHistory::Definition{ address, m_Objects[object].Extent( i ), false };
                    }
                    else if ( kind == Kind::ReadOnly && IsIdentityConstant( s.Name ) )
                    {
                        Defined.Latest[key] =
                             PatchHistory::Definition{ address, m_Objects[object].Extent( i ), false };
                    }
                }
            }
            for ( const auto& [object, symbol] : m_InitialiserSymbols )
            {
                const Coff::Symbol& s = m_Objects[object].Symbols()[symbol];
                Initialisers.push_back( reinterpret_cast<void ( * )()>(
                     AddressOf( object, std::uint32_t( s.Section ) - 1, s.Value ) ) );
            }
            m_Report.PatchBegin = m_Report.PatchBegin == 0 ? Begin : std::min( m_Report.PatchBegin, Begin );
            m_Report.PatchEnd   = std::max( m_Report.PatchEnd, End );
            return true;
        }

        Common::BoolResultStr Linker::Link()
        {
            if ( m_Image.Base == 0 || m_Image.Map == nullptr )
            {
                return Common::MakeError<bool>( "the image to patch is not loaded or has no map" );
            }
            const std::size_t count = m_Objects.size();
            m_Kinds.resize( count );
            m_Decisions.resize( count );
            m_Included.resize( count );
            m_Pieces.resize( count );
            m_Associates.resize( count );
            for ( std::uint32_t object = 0; object < count; ++object )
            {
                const std::span<const Coff::Section> sections = m_Objects[object].Sections();
                const std::span<const Coff::Symbol>  symbols  = m_Objects[object].Symbols();
                m_Kinds[object].reserve( sections.size() );
                for ( const Coff::Section& section : sections )
                {
                    m_Kinds[object].push_back( Classify( section ) );
                }
                m_Decisions[object].assign( symbols.size(), Decision{} );
                m_Included[object].assign( sections.size(), false );
                m_Associates[object].resize( sections.size() );
                SplitSections( object );
                for ( std::uint32_t section = 0; section < sections.size(); ++section )
                {
                    const Coff::Section& s = sections[section];
                    if ( s.Comdat == Coff::ComdatSelect::Associative && s.AssociatedWith >= 1 &&
                         s.AssociatedWith <= sections.size() )
                    {
                        m_Associates[object][s.AssociatedWith - 1].push_back( section );
                    }
                }
                // The first definition of an external wins, as /Gy COMDAT selection "any" does.
                for ( std::uint32_t i = 0; i < symbols.size(); ++i )
                {
                    const Coff::Symbol& s = symbols[i];
                    if ( s.StorageClass == kClassExternal && s.Section > 0 &&
                         m_Kinds[object][std::uint32_t( s.Section ) - 1] != Kind::Discard )
                    {
                        m_Externals.try_emplace( s.Name, object, i );
                    }
                }
            }

            // Roots: every function carrying edited code.
            for ( std::uint32_t object = 0; object < count; ++object )
            {
                const std::span<const Coff::Symbol> symbols = m_Objects[object].Symbols();
                for ( std::uint32_t i = 0; i < symbols.size(); ++i )
                {
                    const Coff::Symbol& s = symbols[i];
                    if ( s.Section <= 0 || !s.Function || IsLocal( s ) ||
                         m_Kinds[object][std::uint32_t( s.Section ) - 1] != Kind::Code || !IsChosen( object, i ) ||
                         m_Carrying.count( NormaliseAnonymousNamespace( s.Name ) ) == 0 )
                    {
                        continue;
                    }
                    Include( object, std::uint32_t( s.Section ) - 1, s.Value );
                }
            }
            if ( !ReadInitialisers() || !Drain() || !IncludePData() || !CheckThrowInfo() )
            {
                return Common::MakeError<bool>( m_Error );
            }
            if ( !Layout() )
            {
                return Common::MakeError<bool>( m_Error );
            }
            const auto release = [&]()
            {
                VirtualFree( reinterpret_cast<void*>( Begin ), 0, MEM_RELEASE );
                Begin = End = 0;
            };
            for ( std::uint32_t object = 0; object < count; ++object )
            {
                for ( std::uint32_t section = 0; section < m_Included[object].size(); ++section )
                {
                    if ( !m_Included[object][section] )
                    {
                        continue;
                    }
                    for ( const Piece& piece : m_Pieces[object][section] )
                    {
                        if ( piece.Included && !Relocate( object, section, piece ) )
                        {
                            release();
                            return Common::MakeError<bool>( m_Error );
                        }
                    }
                }
            }
            if ( !Report() )
            {
                release();
                return Common::MakeError<bool>( m_Error );
            }

            // Unwind: one RUNTIME_FUNCTION table for the whole patch, sorted for the unwinder's binary search,
            // image-relative to Begin like every ADDR32NB above.
            auto* const table = reinterpret_cast<RUNTIME_FUNCTION*>( m_RegionBegin[std::size_t( Region::PData )] );
            const std::size_t entries =
                 ( m_PDataEnd - m_RegionBegin[std::size_t( Region::PData )] ) / sizeof( RUNTIME_FUNCTION );
            std::sort( table, table + entries, []( const RUNTIME_FUNCTION& a, const RUNTIME_FUNCTION& b )
                       { return a.BeginAddress < b.BeginAddress; } );

            const auto protect = [&]( Region region, DWORD rights )
            {
                const std::size_t r        = std::size_t( region );
                DWORD             previous = 0;
                return VirtualProtect( reinterpret_cast<void*>( m_RegionBegin[r] ),
                                       m_RegionBegin[r + 1] - m_RegionBegin[r], rights, &previous ) != FALSE;
            };
            if ( !protect( Region::Code, PAGE_EXECUTE_READ ) || !protect( Region::ReadOnly, PAGE_READONLY ) ||
                 !protect( Region::PData, PAGE_READONLY ) )
            {
                const DWORD error = GetLastError();
                release();
                return Common::MakeFormattedError<bool>( "VirtualProtect of the patch failed ({})", error );
            }
            FlushInstructionCache( GetCurrentProcess(), reinterpret_cast<void*>( Begin ), End - Begin );
            if ( entries != 0 && !RtlAddFunctionTable( table, DWORD( entries ), DWORD64( Begin ) ) )
            {
                release();
                return Common::MakeError<bool>( "RtlAddFunctionTable refused the patch's unwind table" );
            }
            return Common::MakeSuccess( true );
        }
    } // namespace

    OriginalObject::OriginalObject( Coff::Object object ) : m_Object( std::move( object ) )
    {
        const std::span<const Coff::Symbol> symbols = m_Object.Symbols();
        m_ByName.reserve( symbols.size() );
        for ( std::uint32_t i = 0; i < symbols.size(); ++i )
        {
            if ( symbols[i].Section > 0 && !IsLocal( symbols[i] ) )
            {
                m_ByName.try_emplace( NormaliseAnonymousNamespace( symbols[i].Name ), i );
            }
        }
    }

    std::optional<std::uint32_t> OriginalObject::Find( const std::string& normalised ) const
    {
        const auto it = m_ByName.find( normalised );
        return it == m_ByName.end() ? std::nullopt : std::optional<std::uint32_t>( it->second );
    }

    Common::ResultStr<PatchImage> PatchImage::Link( std::span<const Coff::Object>          objects,
                                                    const ImageSymbols&                    image,
                                                    const std::unordered_set<std::string>& carrying,
                                                    PatchReport&                           report )
    {
        Linker                      linker( objects, image, carrying, report );
        const Common::BoolResultStr linked = linker.Link();
        if ( !linked )
        {
            return Common::MakeError<PatchImage>( linked.GetError() );
        }
        PatchImage patch;
        patch.m_Begin        = linker.Begin;
        patch.m_End          = linker.End;
        patch.m_Redirects    = std::move( linker.Redirects );
        patch.m_Initialisers = std::move( linker.Initialisers );
        patch.m_Defined      = std::move( linker.Defined );
        return Common::MakeSuccess( std::move( patch ) );
    }
} // namespace Desert::Editor::LiveCoding
