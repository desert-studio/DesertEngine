#include "PatchImage.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

// The in-memory linker of a Live Coding patch (Live++ model). The objects of the recompiled sources are laid out
// in one allocation near the image they patch; every symbol resolves either into that allocation or into the image
// as it runs (its /MAP, its import table, the exports of the other loaded images). Functions carrying edited code
// are laid out new; header and template code that carries none keeps calling the image's copy, so a function-local
// static inside it stays one object; writable data the image has stays the image's; constants are the new ones.
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
        constexpr std::uint8_t   kClassLabel        = 6;
        constexpr std::uint8_t   kClassWeakExternal = 105;
        constexpr std::uint32_t  kNoSymbol          = ~0u;
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

            bool           Decide( std::uint32_t object, std::uint32_t symbol, Decision& out );
            bool           CheckDataSize( std::uint32_t object, std::uint32_t symbol );
            bool           Include( std::uint32_t object, std::uint32_t section );
            bool           Process( std::uint32_t object, std::uint32_t section );
            bool           Drain();
            bool           ReadInitialisers();
            bool           SameAsOriginal( std::uint32_t object, std::uint32_t symbol );
            bool           IncludePData();
            bool           CheckWritableSections();
            bool           Layout();
            bool           Relocate( std::uint32_t object, std::uint32_t section );
            std::uintptr_t Thunk( std::uintptr_t target );
            std::uintptr_t TargetOf( const Decision& decision, bool imageRelative );
            void           Report();

            std::span<const Coff::Object>   m_Objects;
            const ImageSymbols&             m_Image;
            PatchReport&                    m_Report;
            std::unordered_set<std::string> m_Carrying; // normalised
            std::string                     m_Error;

            std::vector<std::vector<Kind>>                       m_Kinds;
            std::vector<std::vector<Decision>>                   m_Decisions;
            std::vector<std::vector<bool>>                       m_Included;
            std::vector<std::vector<std::uintptr_t>>             m_SectionAddress;
            std::vector<std::vector<std::vector<std::uint32_t>>> m_Associates; // parent section -> associatives
            std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>> m_Externals;
            std::vector<std::unordered_map<std::string, std::uint32_t>> m_OriginalByName; // lazily, per object
            std::set<std::pair<std::uint32_t, std::uint32_t>>    m_LocalCopies; // image data copied for ADDR32NB
            std::vector<std::pair<std::uint32_t, std::uint32_t>> m_Work;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> m_InitialiserSymbols;

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
                            return Fail( "a new thread_local (" + s.Name + ") needs a rebuild" );
                        }
                        decision.Place   = Where::Image;
                        decision.Address = m_Image.Base + *rva;
                        break;
                    }
                    case Kind::Code:
                        decision.Place = Where::Patch;
                        if ( !IsLocal( s ) && m_Carrying.count( NormaliseAnonymousNamespace( s.Name ) ) == 0 )
                        {
                            if ( const auto rva = MapRva( object, s.Name ) )
                            {
                                decision.Place   = Where::Image; // carries no edited code: the image's copy stays
                                decision.Address = m_Image.Base + *rva;
                            }
                        }
                        break;
                    case Kind::ReadOnly:
                    case Kind::PData:
                        decision.Place = Where::Patch;
                        break; // constants: the edited values
                    case Kind::Data:
                        decision.Place = Where::Patch;
                        if ( !IsLocal( s ) )
                        {
                            if ( const auto rva = MapRva( object, s.Name ) )
                            {
                                if ( !CheckDataSize( object, symbol ) )
                                {
                                    return false;
                                }
                                decision.Place   = Where::Image; // live data stays where it is
                                decision.Address = m_Image.Base + *rva;
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
                else
                {
                    const std::uintptr_t address = Export( s.Name );
                    if ( address == 0 )
                    {
                        return Fail( "unresolved symbol " + s.Name + ": not in the image's map nor exported" );
                    }
                    decision.Place   = Where::Image;
                    decision.Address = address;
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

        bool Linker::CheckDataSize( std::uint32_t object, std::uint32_t symbol )
        {
            if ( object >= m_Image.Originals.size() || m_Image.Originals[object] == nullptr )
            {
                return true; // compiled alone before too and not reloaded: nothing to compare against
            }
            const Coff::Object& original = *m_Image.Originals[object];
            auto&               byName   = m_OriginalByName[object];
            if ( byName.empty() )
            {
                const std::span<const Coff::Symbol> symbols = original.Symbols();
                for ( std::uint32_t i = 0; i < symbols.size(); ++i )
                {
                    if ( symbols[i].Section > 0 && !symbols[i].Function && !IsLocal( symbols[i] ) )
                    {
                        byName.try_emplace( NormaliseAnonymousNamespace( symbols[i].Name ), i );
                    }
                }
            }
            const Coff::Symbol& s  = m_Objects[object].Symbols()[symbol];
            const auto          it = byName.find( NormaliseAnonymousNamespace( s.Name ) );
            if ( it == byName.end() )
            {
                return true; // defined by another object of the image: its size is not this object's to judge
            }
            const std::uint32_t before = original.Extent( it->second );
            const std::uint32_t after  = m_Objects[object].Extent( symbol );
            if ( after > before )
            {
                return Fail( "data layout changed: " + s.Name + " grew from " + std::to_string( before ) + " to " +
                             std::to_string( after ) + " bytes; live data cannot move (rebuild and restart)" );
            }
            return true;
        }

        bool Linker::Include( std::uint32_t object, std::uint32_t section )
        {
            const Kind kind = m_Kinds[object][section];
            if ( m_Included[object][section] || kind == Kind::Discard || kind == Kind::Tls ||
                 kind == Kind::CrtInit || kind == Kind::Ignored )
            {
                return true;
            }
            m_Included[object][section] = true;
            m_Work.emplace_back( object, section );
            for ( const std::uint32_t child : m_Associates[object][section] )
            {
                Include( object, child );
            }
            return true;
        }

        bool Linker::Process( std::uint32_t object, std::uint32_t section )
        {
            const Coff::Section& s = m_Objects[object].Sections()[section];
            for ( const Coff::Relocation& relocation : s.Relocations )
            {
                Decision decision;
                if ( !Decide( object, relocation.Symbol, decision ) )
                {
                    return false;
                }
                const bool imageRelative = relocation.Kind == Coff::RelocationKind::Addr32Nb;
                const bool rel32         = relocation.Kind >= Coff::RelocationKind::Rel32 &&
                                   relocation.Kind <= Coff::RelocationKind::Rel32_5;
                if ( decision.Place == Where::Image && imageRelative && decision.DefSymbol != kNoSymbol &&
                     m_Kinds[decision.DefObject]
                            [std::uint32_t( m_Objects[decision.DefObject].Symbols()[decision.DefSymbol].Section ) -
                             1] == Kind::Data )
                {
                    // Image-relative from the patch's base (EH tables -> TypeDescriptor): the patch's own copy.
                    m_LocalCopies.emplace( decision.DefObject, decision.DefSymbol );
                    decision.Place = Where::Patch;
                }
                if ( decision.Place == Where::Patch )
                {
                    const Coff::Symbol& target = m_Objects[decision.DefObject].Symbols()[decision.DefSymbol];
                    Include( decision.DefObject, std::uint32_t( target.Section ) - 1 );
                }
                else if ( decision.Place == Where::Slot )
                {
                    m_SlotTargets.insert( decision.Address );
                }
                else if ( imageRelative || rel32 )
                {
                    m_ThunkTargets.insert( decision.Address );
                }
            }
            return true;
        }

        bool Linker::Drain()
        {
            while ( !m_Work.empty() )
            {
                const auto [object, section] = m_Work.back();
                m_Work.pop_back();
                if ( !Process( object, section ) )
                {
                    return false;
                }
            }
            return true;
        }

        bool Linker::SameAsOriginal( std::uint32_t object, std::uint32_t symbol )
        {
            if ( object >= m_Image.Originals.size() || m_Image.Originals[object] == nullptr )
            {
                return false;
            }
            const Coff::Object& original = *m_Image.Originals[object];
            const std::string   name     = NormaliseAnonymousNamespace( m_Objects[object].Symbols()[symbol].Name );
            for ( const Coff::Symbol& candidate : original.Symbols() )
            {
                if ( candidate.Section <= 0 || !candidate.Function ||
                     NormaliseAnonymousNamespace( candidate.Name ) != name )
                {
                    continue;
                }
                const Coff::Section& before = original.Sections()[std::size_t( candidate.Section ) - 1];
                const Coff::Section& after =
                     m_Objects[object].Sections()[std::size_t( m_Objects[object].Symbols()[symbol].Section ) - 1];
                return before.Size == after.Size && before.Relocations.size() == after.Relocations.size() &&
                       std::equal( before.Bytes.begin(), before.Bytes.end(), after.Bytes.begin(),
                                   after.Bytes.end() );
            }
            return false;
        }

        // The .CRT$XCU entries: one `??__E<variable>` per dynamically initialised variable. The initialiser of a
        // variable the image lacks runs after the redirects; one of a variable the image has is never rerun (it
        // would reinitialise live data), and is reported when its code changed.
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
                            Include( object, std::uint32_t( symbols[initialiser].Section ) - 1 );
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
                        Include( object, section );
                    }
                }
            }
            return Drain();
        }

        bool Linker::CheckWritableSections()
        {
            for ( std::uint32_t object = 0; object < m_Objects.size(); ++object )
            {
                const std::span<const Coff::Symbol> symbols = m_Objects[object].Symbols();
                for ( std::uint32_t i = 0; i < symbols.size(); ++i )
                {
                    const Coff::Symbol& s = symbols[i];
                    if ( s.Section <= 0 || IsLocal( s ) || !m_Included[object][std::uint32_t( s.Section ) - 1] ||
                         m_Kinds[object][std::uint32_t( s.Section ) - 1] != Kind::Data || !IsChosen( object, i ) ||
                         m_LocalCopies.count( { object, i } ) != 0 )
                    {
                        continue;
                    }
                    Decision decision;
                    if ( !Decide( object, i, decision ) )
                    {
                        return false;
                    }
                    if ( decision.Place == Where::Image )
                    {
                        // The section is laid out for a neighbour, so this variable would exist twice.
                        return Fail( "data layout changed: " + s.Name + " shares a section with data the image " +
                                     "lacks; live data cannot be split (rebuild and restart)" );
                    }
                }
            }
            return true;
        }

        bool Linker::Layout()
        {
            std::uintptr_t                           size[std::size_t( Region::Count )] = {};
            std::vector<std::vector<std::uintptr_t>> offsets( m_Objects.size() );
            for ( std::uint32_t object = 0; object < m_Objects.size(); ++object )
            {
                const std::span<const Coff::Section> sections = m_Objects[object].Sections();
                offsets[object].assign( sections.size(), 0 );
                for ( std::uint32_t section = 0; section < sections.size(); ++section )
                {
                    if ( !m_Included[object][section] )
                    {
                        continue;
                    }
                    std::uintptr_t&     end = size[std::size_t( RegionOf( m_Kinds[object][section] ) )];
                    const std::uint32_t alignment =
                         m_Kinds[object][section] == Kind::PData ? 4u : AlignmentOf( sections[section] );
                    end                      = AlignUp( end, alignment );
                    offsets[object][section] = end;
                    end += sections[section].Size;
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

            for ( std::uint32_t object = 0; object < m_Objects.size(); ++object )
            {
                const std::span<const Coff::Section> sections = m_Objects[object].Sections();
                for ( std::uint32_t section = 0; section < sections.size(); ++section )
                {
                    if ( !m_Included[object][section] )
                    {
                        continue;
                    }
                    const std::uintptr_t address =
                         m_RegionBegin[std::size_t( RegionOf( m_Kinds[object][section] ) )] +
                         offsets[object][section];
                    m_SectionAddress[object][section] = address;
                    if ( !sections[section]
                               .Bytes.empty() ) // uninitialised data stays the zeroes VirtualAlloc gave
                    {
                        std::memcpy( reinterpret_cast<void*>( address ), sections[section].Bytes.data(),
                                     sections[section].Bytes.size() );
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

        std::uintptr_t Linker::TargetOf( const Decision& decision, bool imageRelative )
        {
            const bool localCopy = imageRelative && decision.DefSymbol != kNoSymbol &&
                                   m_LocalCopies.count( { decision.DefObject, decision.DefSymbol } ) != 0;
            if ( decision.Place == Where::Patch || localCopy )
            {
                const Coff::Symbol& s = m_Objects[decision.DefObject].Symbols()[decision.DefSymbol];
                return m_SectionAddress[decision.DefObject][std::uint32_t( s.Section ) - 1] + s.Value;
            }
            if ( decision.Place == Where::Slot )
            {
                return m_Slots.at( decision.Address );
            }
            return decision.Address;
        }

        bool Linker::Relocate( std::uint32_t object, std::uint32_t section )
        {
            const Coff::Section& s    = m_Objects[object].Sections()[section];
            const std::uintptr_t base = m_SectionAddress[object][section];
            for ( const Coff::Relocation& relocation : s.Relocations )
            {
                const std::uintptr_t at    = base + relocation.Offset;
                const std::size_t    width = relocation.Kind == Coff::RelocationKind::Addr64 ? 8 : 4;
                if ( std::size_t( relocation.Offset ) + width > s.Size )
                {
                    return Fail( s.Name + ": a relocation lies past the section end" );
                }
                Decision decision;
                if ( !Decide( object, relocation.Symbol, decision ) )
                {
                    return false;
                }
                const std::string& name = m_Objects[object].Symbols()[relocation.Symbol].Name;
                switch ( relocation.Kind )
                {
                    case Coff::RelocationKind::Addr64:
                    {
                        std::uint64_t addend = 0;
                        std::memcpy( &addend, reinterpret_cast<void*>( at ), 8 );
                        const std::uint64_t value = TargetOf( decision, false ) + addend;
                        std::memcpy( reinterpret_cast<void*>( at ), &value, 8 );
                        break;
                    }
                    case Coff::RelocationKind::Addr32Nb:
                    {
                        std::int32_t addend = 0;
                        std::memcpy( &addend, reinterpret_cast<void*>( at ), 4 );
                        std::uintptr_t target = TargetOf( decision, true );
                        if ( target < Begin || target >= End )
                        {
                            if ( !decision.Function )
                            {
                                return Fail( "image-relative reference from the patch to data outside it: " +
                                             name );
                            }
                            target = Thunk(
                                 target ); // the EH handler (__CxxFrameHandler4) and other runtime functions
                            if ( target == 0 )
                            {
                                return Fail( "the patch ran out of thunks resolving " + name );
                            }
                        }
                        const std::int64_t value = std::int64_t( target - Begin ) + addend;
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
                        std::int32_t addend = 0;
                        std::memcpy( &addend, reinterpret_cast<void*>( at ), 4 );
                        std::uintptr_t target = TargetOf( decision, false );
                        std::int64_t   value = std::int64_t( target ) + addend - std::int64_t( at + 4 + trailing );
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
                            value = std::int64_t( target ) + addend - std::int64_t( at + 4 + trailing );
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
                        std::int32_t addend = 0;
                        std::memcpy( &addend, reinterpret_cast<void*>( at ), 4 );
                        const std::uint32_t written =
                             std::uint32_t( decision.Address - m_Image.Base - *tls ) + std::uint32_t( addend );
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

        void Linker::Report()
        {
            for ( std::uint32_t object = 0; object < m_Objects.size(); ++object )
            {
                const std::span<const Coff::Symbol> symbols = m_Objects[object].Symbols();
                for ( std::uint32_t i = 0; i < symbols.size(); ++i )
                {
                    const Coff::Symbol& s = symbols[i];
                    if ( s.Section <= 0 || IsLocal( s ) || !m_Included[object][std::uint32_t( s.Section ) - 1] ||
                         !IsChosen( object, i ) )
                    {
                        continue;
                    }
                    const Kind           kind = m_Kinds[object][std::uint32_t( s.Section ) - 1];
                    const std::uintptr_t address =
                         m_SectionAddress[object][std::uint32_t( s.Section ) - 1] + s.Value;
                    if ( kind == Kind::Code && s.Function )
                    {
                        const bool carrying = m_Carrying.count( NormaliseAnonymousNamespace( s.Name ) ) != 0;
                        const auto rva      = MapRva( object, s.Name );
                        if ( carrying && rva )
                        {
                            Redirects.push_back( Redirect{ m_Image.Base + *rva, address, s.Name } );
                            m_Report.Redirected.push_back( s.Name );
                        }
                        else if ( !rva )
                        {
                            m_Report.NewFunctions.push_back( s.Name );
                        }
                    }
                    else if ( kind == Kind::Data && m_LocalCopies.count( { object, i } ) == 0 )
                    {
                        m_Report.NewData.push_back( s.Name );
                    }
                }
            }
            for ( const auto& [object, symbol] : m_InitialiserSymbols )
            {
                const Coff::Symbol& s = m_Objects[object].Symbols()[symbol];
                Initialisers.push_back( reinterpret_cast<void ( * )()>(
                     m_SectionAddress[object][std::uint32_t( s.Section ) - 1] + s.Value ) );
            }
            m_Report.PatchBegin = m_Report.PatchBegin == 0 ? Begin : std::min( m_Report.PatchBegin, Begin );
            m_Report.PatchEnd   = std::max( m_Report.PatchEnd, End );
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
            m_SectionAddress.resize( count );
            m_Associates.resize( count );
            m_OriginalByName.resize( count );
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
                m_SectionAddress[object].assign( sections.size(), 0 );
                m_Associates[object].resize( sections.size() );
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
                    Include( object, std::uint32_t( s.Section ) - 1 );
                }
            }
            if ( !ReadInitialisers() || !Drain() || !IncludePData() || !CheckWritableSections() )
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
                    if ( m_Included[object][section] && !Relocate( object, section ) )
                    {
                        release();
                        return Common::MakeError<bool>( m_Error );
                    }
                }
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
            Report();
            return Common::MakeSuccess( true );
        }
    } // namespace

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
        return Common::MakeSuccess( std::move( patch ) );
    }
} // namespace Desert::Editor::LiveCoding
