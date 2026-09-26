#include <Common/Utilities/AssetRegistry.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>

namespace Common::Utils
{
    namespace
    {
        constexpr std::string_view kMagic         = "DesertAssetRegistry";
        constexpr int              kFormatVersion = 4;
        // The file's name under the Cooked tree. One spelling, here, because both the producer (the
        // editor's cook) and the consumer (both hosts' boot) have to name the same file and a second
        // literal is how they would come to name two.
        constexpr std::string_view kFileName = "AssetRegistry.dreg";

        // `-` rather than `0` for "no identity" and "no dependencies". Zero is a legal-looking number
        // and would read as an identity of zero — which is what a NULL handle is — so the empty case
        // and the null case would be spelled the same. A dash cannot be mistaken for either.
        constexpr std::string_view kNone = "-";

        std::string HexU64( uint64_t value )
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string           out( 16, '0' );
            for ( int i = 15; i >= 0; --i )
            {
                out[static_cast<std::size_t>( i )] = kDigits[value & 0xFull];
                value >>= 4;
            }
            return out;
        }

        std::string HexU32( uint32_t value )
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string           out( 8, '0' );
            for ( int i = 7; i >= 0; --i )
            {
                out[static_cast<std::size_t>( i )] = kDigits[value & 0xFu];
                value >>= 4;
            }
            return out;
        }

        // The six floats of a box, min then max, in the order the column states them.
        std::array<float, 6> BoxFloats( const Common::Math::AABB& box )
        {
            return { box.Min.x, box.Min.y, box.Min.z, box.Max.x, box.Max.y, box.Max.z };
        }

        std::string BoundsText( const Common::Math::AABB& box )
        {
            std::string out;
            out.reserve( std::size_t{ 6 } * 9 );
            const std::array<float, 6> values = BoxFloats( box );
            for ( std::size_t i = 0; i < values.size(); ++i )
            {
                if ( i != 0 )
                    out += ',';
                out += HexU32( std::bit_cast<uint32_t>( values[i] ) );
            }
            return out;
        }

        bool ParseHexU32( std::string_view text, uint32_t& out )
        {
            if ( text.size() != 8 )
                return false;
            uint32_t value = 0;
            for ( const char c : text )
            {
                value <<= 4;
                if ( c >= '0' && c <= '9' )
                    value |= static_cast<uint32_t>( c - '0' );
                else if ( c >= 'a' && c <= 'f' )
                    value |= static_cast<uint32_t>( c - 'a' + 10 );
                else
                    return false;
            }
            out = value;
            return true;
        }

        // Exactly six 8-hex fields; a NaN or an inverted box is refused, because a box no point is inside
        // would place a record nowhere and look like data while doing it.
        bool ParseBounds( std::string_view text, Common::Math::AABB& out )
        {
            std::array<float, 6> values{};
            for ( std::size_t i = 0; i < values.size(); ++i )
            {
                const std::size_t comma = text.find( ',' );
                if ( ( i + 1 < values.size() ) == ( comma == std::string_view::npos ) )
                    return false;
                uint32_t bits = 0;
                if ( !ParseHexU32( text.substr( 0, comma ), bits ) )
                    return false;
                values[i] = std::bit_cast<float>( bits );
                if ( !std::isfinite( values[i] ) )
                    return false;
                text = comma == std::string_view::npos ? std::string_view() : text.substr( comma + 1 );
            }
            out.Min = glm::vec3( values[0], values[1], values[2] );
            out.Max = glm::vec3( values[3], values[4], values[5] );
            return out.Min.x <= out.Max.x && out.Min.y <= out.Max.y && out.Min.z <= out.Max.z;
        }

        std::string HeaderText( const AssetRegistryEntry& entry )
        {
            if ( !entry.Guid )
                return std::string( kNone );
            std::string out = Content::AssetGuidToText( *entry.Guid );
            out += ';';
            std::vector<Content::SubsystemVersion> versions = entry.Versions;
            std::sort( versions.begin(), versions.end(),
                       []( const Content::SubsystemVersion& a, const Content::SubsystemVersion& b )
                       { return a.Tag < b.Tag; } );
            for ( std::size_t i = 0; i < versions.size(); ++i )
            {
                if ( i != 0 )
                    out += ',';
                out += Content::FourCCToString( versions[i].Tag );
                out += '=';
                out += std::to_string( versions[i].Version );
            }
            return out;
        }

        // `<guid>;<TAG>=<n>,...` back into the entry; false on any malformed piece.
        bool ParseHeaderText( std::string_view text, AssetRegistryEntry& entry )
        {
            const std::size_t semicolon = text.find( ';' );
            if ( semicolon == std::string_view::npos )
                return false;
            const auto guid = Content::AssetGuidFromText( text.substr( 0, semicolon ) );
            if ( !guid || guid.GetValue().IsNull() )
                return false;
            entry.Guid            = guid.GetValue();
            std::string_view rest = text.substr( semicolon + 1 );
            while ( !rest.empty() )
            {
                const std::size_t      comma = rest.find( ',' );
                const std::string_view one   = rest.substr( 0, comma );
                if ( one.size() < 6 || one[4] != '=' )
                    return false;
                Content::SubsystemVersion version;
                for ( std::size_t i = 0; i < 4; ++i )
                    version.Tag |= static_cast<uint32_t>( static_cast<unsigned char>( one[i] ) ) << ( 8 * i );
                const std::string_view number = one.substr( 5 );
                const auto             parsed =
                     std::from_chars( number.data(), number.data() + number.size(), version.Version );
                if ( parsed.ec != std::errc() || parsed.ptr != number.data() + number.size() )
                    return false;
                entry.Versions.push_back( version );
                if ( comma == std::string_view::npos )
                    break;
                rest = rest.substr( comma + 1 );
            }
            return true;
        }

        bool ParseHexU64( std::string_view text, uint64_t& out )
        {
            if ( text.size() != 16 )
                return false;
            uint64_t value = 0;
            for ( const char c : text )
            {
                value <<= 4;
                if ( c >= '0' && c <= '9' )
                    value |= static_cast<uint64_t>( c - '0' );
                else if ( c >= 'a' && c <= 'f' )
                    value |= static_cast<uint64_t>( c - 'a' + 10 );
                else
                    return false;
            }
            out = value;
            return true;
        }

        // A tag value may hold any byte; the column is space-delimited and its entries comma-separated, so
        // those two, the separator `=`, the escape itself and every non-printing byte are written %XX.
        bool TagByteIsEscaped( unsigned char c )
        {
            return c <= 0x20 || c == 0x7f || c == ',' || c == '=' || c == '%';
        }

        std::string EscapeTagValue( std::string_view value )
        {
            static constexpr char kDigits[] = "0123456789ABCDEF";
            std::string           out;
            for ( const char ch : value )
            {
                const auto c = static_cast<unsigned char>( ch );
                if ( !TagByteIsEscaped( c ) )
                {
                    out += ch;
                    continue;
                }
                out += '%';
                out += kDigits[c >> 4];
                out += kDigits[c & 0xf];
            }
            return out;
        }

        bool UnescapeTagValue( std::string_view text, std::string& out )
        {
            out.clear();
            for ( std::size_t i = 0; i < text.size(); ++i )
            {
                if ( text[i] != '%' )
                {
                    out += text[i];
                    continue;
                }
                if ( i + 2 >= text.size() )
                    return false;
                unsigned int byte   = 0;
                const auto   parsed = std::from_chars( text.data() + i + 1, text.data() + i + 3, byte, 16 );
                if ( parsed.ec != std::errc() || parsed.ptr != text.data() + i + 3 )
                    return false;
                out += static_cast<char>( byte );
                i += 2;
            }
            return true;
        }

        constexpr std::string_view kTagName    = "Name";
        constexpr std::string_view kTagSkinned = "Skinned";

        // `Name=<escaped>` and `Skinned`, comma separated in that order, or `-` when the file states neither.
        std::string TagsText( const AssetRegistryEntry& entry )
        {
            std::string out;
            if ( !entry.DisplayName.empty() )
            {
                out += kTagName;
                out += '=';
                out += EscapeTagValue( entry.DisplayName );
            }
            if ( entry.Skinned )
            {
                if ( !out.empty() )
                    out += ',';
                out += kTagSkinned;
            }
            return out.empty() ? std::string( kNone ) : out;
        }

        // An unknown tag is refused, not skipped: a reader that dropped it would serve rows missing a column
        // the writer meant them to carry, which is the silent read the version line exists to prevent.
        bool ParseTags( std::string_view text, AssetRegistryEntry& entry )
        {
            for ( ;; )
            {
                const std::size_t      comma = text.find( ',' );
                const std::string_view one   = text.substr( 0, comma );
                if ( one == kTagSkinned )
                    entry.Skinned = true;
                else if ( one.starts_with( kTagName ) && one.size() > kTagName.size() + 1 &&
                          one[kTagName.size()] == '=' )
                {
                    if ( !UnescapeTagValue( one.substr( kTagName.size() + 1 ), entry.DisplayName ) )
                        return false;
                }
                else
                    return false;
                if ( comma == std::string_view::npos )
                    return true;
                text = text.substr( comma + 1 );
            }
        }

        // Splits off the next whitespace-delimited column, leaving `rest` at the start of the one
        // after it. Returns false when there is no column left, which is how every "line N has no
        // <column>" refusal below is produced rather than by counting spaces twice.
        bool NextColumn( std::string_view& rest, std::string_view& column )
        {
            const std::size_t end = rest.find( ' ' );
            if ( end == std::string_view::npos )
                return false;
            column = rest.substr( 0, end );
            rest   = rest.substr( end + 1 );
            return true;
        }
    } // namespace

    uint64_t AssetRegistryEntry::PathHandle() const
    {
        return static_cast<uint64_t>( AssetHandle::FromKey( Key ) );
    }

    uint64_t AssetRegistryEntry::EffectiveHandle() const
    {
        return Identity != 0 ? Identity : PathHandle();
    }

    BoolResultStr AssetRegistry::Insert( AssetRegistryEntry entry )
    {
        if ( entry.Key.empty() )
            return MakeError<bool>( "a registry row with no key names nothing" );
        // A key with a space in it round-trips (the key column is last) but a key with a NEWLINE
        // cannot: it would be read back as two rows, the second of them malformed. Legal on POSIX,
        // so it is refused at the door rather than silently mangled — ContentManifest::FromDirectory
        // makes the same refusal for the same reason.
        if ( entry.Key.find( '\n' ) != std::string::npos || entry.Key.find( '\r' ) != std::string::npos )
            return MakeFormattedError<bool>( "the key '{}' contains a line break and cannot be written "
                                             "as one row",
                                             entry.Key );
        if ( entry.Kind.empty() || entry.Kind.find( ' ' ) != std::string::npos )
            return MakeFormattedError<bool>( "'{}' is not a usable kind for '{}': a kind is one "
                                             "non-empty word, because it is a middle column",
                                             entry.Kind, entry.Key );

        const auto at = std::lower_bound( m_Entries.begin(), m_Entries.end(), entry.Key,
                                          []( const AssetRegistryEntry& row, const std::string& key )
                                          { return row.Key < key; } );
        if ( at != m_Entries.end() && at->Key == entry.Key )
            return MakeFormattedError<bool>( "'{}' is already a row in this registry; a file enumerated "
                                             "twice would make the registry depend on walk order",
                                             entry.Key );

        const uint64_t pathHandle = entry.PathHandle();
        const uint64_t identity   = entry.Identity;

        m_KeyByHandle[pathHandle] = entry.Key;
        if ( identity != 0 )
            m_KeyByHandle[identity] = entry.Key;

        m_Entries.insert( at, std::move( entry ) );
        return MakeSuccess( true );
    }

    bool AssetRegistry::Remove( std::string_view key )
    {
        const auto at = std::lower_bound( m_Entries.begin(), m_Entries.end(), key,
                                          []( const AssetRegistryEntry& row, std::string_view wanted )
                                          { return std::string_view( row.Key ) < wanted; } );
        if ( at == m_Entries.end() || at->Key != key )
            return false;

        // BOTH of the row's numbers leave the handle index with it. Erasing only the path-derived one
        // would leave a declared identity pointing at a key that is no longer a row — a lookup that
        // succeeds and then finds nothing, which is worse than one that misses.
        m_KeyByHandle.erase( at->PathHandle() );
        if ( at->Identity != 0 )
            m_KeyByHandle.erase( at->Identity );

        m_Entries.erase( at );
        return true;
    }

    bool AssetRegistry::SetIdentity( std::string_view key, uint64_t identity )
    {
        const auto at = std::lower_bound( m_Entries.begin(), m_Entries.end(), key,
                                          []( const AssetRegistryEntry& row, std::string_view wanted )
                                          { return std::string_view( row.Key ) < wanted; } );
        if ( at == m_Entries.end() || at->Key != key )
            return false;

        if ( at->Identity == identity )
            return true;

        if ( at->Identity != 0 )
            m_KeyByHandle.erase( at->Identity );
        at->Identity = identity;
        if ( identity != 0 )
            m_KeyByHandle[identity] = at->Key;
        return true;
    }

    bool AssetRegistry::SetDependencies( std::string_view key, std::vector<uint64_t> dependencies )
    {
        const auto at = std::lower_bound( m_Entries.begin(), m_Entries.end(), key,
                                          []( const AssetRegistryEntry& row, std::string_view wanted )
                                          { return std::string_view( row.Key ) < wanted; } );
        if ( at == m_Entries.end() || at->Key != key )
            return false;

        at->Dependencies = std::move( dependencies );
        return true;
    }

    bool AssetRegistry::SetBounds( std::string_view key, std::optional<Common::Math::AABB> bounds )
    {
        const auto at = std::lower_bound( m_Entries.begin(), m_Entries.end(), key,
                                          []( const AssetRegistryEntry& row, std::string_view wanted )
                                          { return std::string_view( row.Key ) < wanted; } );
        if ( at == m_Entries.end() || at->Key != key )
            return false;

        at->Bounds = bounds;
        return true;
    }

    const std::vector<AssetRegistryEntry>& AssetRegistry::Entries() const
    {
        return m_Entries;
    }

    std::size_t AssetRegistry::Count() const
    {
        return m_Entries.size();
    }

    bool AssetRegistry::Empty() const
    {
        return m_Entries.empty();
    }

    const AssetRegistryEntry* AssetRegistry::FindByHandle( uint64_t handle ) const
    {
        if ( handle == 0 )
            return nullptr;
        const auto it = m_KeyByHandle.find( handle );
        return it == m_KeyByHandle.end() ? nullptr : FindByKey( it->second );
    }

    namespace
    {
        constexpr std::string_view kRedirectorKind = "Redirector";

        // The chain as a message: "a -> b -> c".
        std::string Chain( const std::vector<std::string_view>& keys )
        {
            std::string text;
            for ( const std::string_view key : keys )
            {
                if ( !text.empty() )
                    text += " -> ";
                text += key;
            }
            return text;
        }
    } // namespace

    ResultStr<const AssetRegistryEntry*> AssetRegistry::FollowRedirectors( const AssetRegistryEntry& row ) const
    {
        const AssetRegistryEntry*     at = &row;
        std::vector<std::string_view> passed;
        while ( at->Kind == kRedirectorKind )
        {
            if ( std::ranges::find( passed, std::string_view( at->Key ) ) != passed.end() )
            {
                passed.emplace_back( at->Key );
                return MakeFormattedError<const AssetRegistryEntry*>( "redirector cycle: {}", Chain( passed ) );
            }
            passed.emplace_back( at->Key );
            if ( passed.size() > kMaxRedirectorChain )
                return MakeFormattedError<const AssetRegistryEntry*>( "redirector chain longer than {} links: {}",
                                                                      kMaxRedirectorChain, Chain( passed ) );
            if ( at->Dependencies.size() != 1 )
                return MakeFormattedError<const AssetRegistryEntry*>(
                     "redirector '{}' has {} target edges, a redirector has one", at->Key,
                     at->Dependencies.size() );
            const AssetRegistryEntry* next = FindByHandle( at->Dependencies.front() );
            if ( next == nullptr )
                return MakeFormattedError<const AssetRegistryEntry*>(
                     "redirector '{}' names asset {:016x}, which no row states (chain: {})", at->Key,
                     at->Dependencies.front(), Chain( passed ) );
            at = next;
        }
        return MakeSuccess( at );
    }

    namespace
    {
        // A broken redirector is not "no such asset": it is logged by name, and the caller gets the absence
        // it already handles for a dangling reference.
        const AssetRegistryEntry* Followed( const AssetRegistry& registry, const AssetRegistryEntry* row )
        {
            if ( row == nullptr || row->Kind != kRedirectorKind )
                return row;
            auto followed = registry.FollowRedirectors( *row );
            if ( !followed )
            {
                LOG_ERROR( "[AssetRegistry] {}", followed.GetError() );
                return nullptr;
            }
            // THE LOCATOR MISSED: the path names where the asset was. One line, not a refusal; AF10d's fix-up
            // rewrites the referrer and deletes the redirector.
            LOG_WARN( "[AssetRegistry] '{}' is a redirector to '{}'", row->Key, followed.GetValue()->Key );
            return followed.GetValue();
        }
    } // namespace

    const AssetRegistryEntry* AssetRegistry::FindByReference( uint64_t handle, std::string_view path ) const
    {
        if ( handle != 0 )
        {
            if ( const AssetRegistryEntry* row = FindByHandle( handle ) )
                return Followed( *this, row );
        }
        if ( path.empty() )
            return nullptr;
        return Followed( *this, FindByKey( AssetHandle::StableKeyForPath( std::filesystem::path( path ) ) ) );
    }

    const AssetRegistryEntry* AssetRegistry::FindByGuidReference( const Content::AssetGuid& guid,
                                                                  std::string_view          path ) const
    {
        if ( !guid.IsNull() )
            for ( const AssetRegistryEntry& row : m_Entries )
                if ( row.Guid.has_value() && *row.Guid == guid )
                {
                    // The GUID answered and the path hint names somewhere else: the asset moved. One line.
                    if ( !path.empty() )
                    {
                        const std::string hinted = AssetHandle::StableKeyForPath( std::filesystem::path( path ) );
                        if ( !hinted.empty() && hinted != row.Key )
                            LOG_WARN( "[AssetRegistry] locator '{}' is stale; its GUID lives at '{}'", hinted,
                                      row.Key );
                    }
                    return Followed( *this, &row );
                }
        return FindByReference( 0, path );
    }

    const AssetRegistryEntry* AssetRegistry::FindByKey( std::string_view key ) const
    {
        const auto at = std::lower_bound( m_Entries.begin(), m_Entries.end(), key,
                                          []( const AssetRegistryEntry& row, std::string_view wanted )
                                          { return std::string_view( row.Key ) < wanted; } );
        if ( at == m_Entries.end() || at->Key != key )
            return nullptr;
        return &*at;
    }

    std::vector<const AssetRegistryEntry*> AssetRegistry::OfKind( std::string_view kind ) const
    {
        std::vector<const AssetRegistryEntry*> rows;
        for ( const AssetRegistryEntry& entry : m_Entries )
        {
            if ( entry.Kind == kind )
                rows.push_back( &entry );
        }
        return rows;
    }

    std::size_t AssetRegistry::PublishIdentities() const
    {
        std::size_t bound = 0;
        for ( const AssetRegistryEntry& entry : m_Entries )
        {
            if ( AssetPathIndex::Record( entry.PathHandle(), entry.Key ) )
                ++bound;

            // THE DECLARED IDENTITY IS DELIBERATELY NOT PUBLISHED HERE, and the first run that did
            // publish it is why the distinction is written down rather than assumed.
            //
            // `AssetPathIndex` answers ONE question: which string was this number hashed from. For a
            // `.tex` that string is the SOURCE image's key (`assets:Meshes/shaded.png`) — `TextureAsset
            // ::Load` records it through `AdoptHandleFromFile` with exactly that argument. Binding the
            // same number to the COOKED file's key here made the index refuse the loader's own record
            // a moment later and print `two files cannot share one identity` twice, over two files
            // that share nothing: one is the other's cooked twin.
            //
            // "Which FILE does this number name" is a different question and it is this registry's,
            // answered by `FindByHandle`, which indexes the declared identity as well as the derived
            // one. Conflating the two would have made the collision refusal — which exists to catch a
            // real and serious defect — fire on the normal case, i.e. would have taught everyone to
            // ignore it.
        }
        return bound;
    }

    std::string AssetRegistry::Serialize() const
    {
        std::string out;
        out.reserve( m_Entries.size() * 96 + 32 );
        out += kMagic;
        out += ' ';
        out += std::to_string( kFormatVersion );
        out += '\n';
        for ( const AssetRegistryEntry& entry : m_Entries )
        {
            out += std::to_string( entry.Size );
            out += ' ';
            out += entry.Kind;
            out += ' ';
            out += HeaderText( entry );
            out += ' ';
            out += entry.Identity == 0 ? std::string( kNone ) : HexU64( entry.Identity );
            out += ' ';
            if ( entry.Dependencies.empty() )
            {
                out += kNone;
            }
            else
            {
                // SORTED AND DEDUPLICATED ON THE WAY OUT rather than trusted from the producer. The
                // edges come from walking an asset's slots, and slot order is a property of the file
                // being read, not of the content: two cooks of one tree must produce one byte string.
                std::vector<uint64_t> deps = entry.Dependencies;
                std::sort( deps.begin(), deps.end() );
                deps.erase( std::unique( deps.begin(), deps.end() ), deps.end() );
                for ( std::size_t i = 0; i < deps.size(); ++i )
                {
                    if ( i != 0 )
                        out += ',';
                    out += HexU64( deps[i] );
                }
            }
            out += ' ';
            // NOLINTBEGIN(bugprone-unchecked-optional-access)
            out += entry.Bounds.has_value() ? BoundsText( entry.Bounds.value() )
                                            : std::string( kNone ); // NOLINT(bugprone-unchecked-optional-access)
            // NOLINTEND(bugprone-unchecked-optional-access)
            out += ' ';
            out += TagsText( entry );
            out += ' ';
            out += entry.Key;
            out += '\n';
        }
        return out;
    }

    ResultStr<AssetRegistry> AssetRegistry::Parse( std::string_view text )
    {
        std::size_t lineStart = 0;
        std::size_t lineNo    = 0;

        auto nextLine = [&]( std::string_view& line ) -> bool
        {
            if ( lineStart >= text.size() )
                return false;
            const std::size_t end = text.find( '\n', lineStart );
            line = text.substr( lineStart, ( end == std::string_view::npos ? text.size() : end ) - lineStart );
            lineStart = ( end == std::string_view::npos ) ? text.size() : end + 1;
            if ( !line.empty() && line.back() == '\r' )
                line.remove_suffix( 1 );
            ++lineNo;
            return true;
        };

        std::string_view header;
        if ( !nextLine( header ) )
            return MakeError<AssetRegistry>( "the asset registry is empty — its header line is missing" );

        // ONE FORM IS READ: the one this build writes. An older form lacks columns (the tags, before 4) that
        // its rows would then silently serve as empty — a picker listing file stems for names the files state.
        // A registry is derived state; the answer to an old one is the cook that rewrites it.
        const std::string expected = std::string( kMagic ) + " " + std::to_string( kFormatVersion );
        if ( header != expected )
        {
            if ( header.starts_with( kMagic ) )
                return MakeFormattedError<AssetRegistry>(
                     R"(an asset registry of another form: line 1 is "{}", this build reads only "{}" — )"
                     "re-cook it (AssetRegistryTool cook)",
                     std::string( header ), expected );
            return MakeFormattedError<AssetRegistry>(
                 R"(not a Desert asset registry: line 1 is "{}", expected "{}")", std::string( header ),
                 expected );
        }

        AssetRegistry    registry;
        std::string_view line;
        while ( nextLine( line ) )
        {
            if ( line.empty() )
                continue;

            std::string_view rest = line;
            std::string_view sizeText;
            std::string_view kindText;
            std::string_view headerText;
            std::string_view identityText;
            std::string_view depsText;
            std::string_view boundsText;
            std::string_view tagsText;
            if ( !NextColumn( rest, sizeText ) || !NextColumn( rest, kindText ) ||
                 !NextColumn( rest, headerText ) || !NextColumn( rest, identityText ) ||
                 !NextColumn( rest, depsText ) || !NextColumn( rest, boundsText ) ||
                 !NextColumn( rest, tagsText ) || rest.empty() )
            {
                return MakeFormattedError<AssetRegistry>(
                     "line {} is not a registry row — expected \"<size> <kind> "
                     "<header> <identity> <deps> <bounds> <tags> <key>\"",
                     lineNo );
            }

            AssetRegistryEntry entry;
            entry.Key = std::string( rest );

            const auto sizeParsed =
                 std::from_chars( sizeText.data(), sizeText.data() + sizeText.size(), entry.Size );
            if ( sizeParsed.ec != std::errc() || sizeParsed.ptr != sizeText.data() + sizeText.size() )
                return MakeFormattedError<AssetRegistry>( "line {}: '{}' is not a size", lineNo,
                                                          std::string( sizeText ) );

            entry.Kind = std::string( kindText );

            if ( headerText != kNone && !ParseHeaderText( headerText, entry ) )
                return MakeFormattedError<AssetRegistry>(
                     "line {}: '{}' is neither '-' nor '<32-hex GUID>;<TAG>=<version>,...'", lineNo,
                     std::string( headerText ) );

            if ( identityText != kNone && !ParseHexU64( identityText, entry.Identity ) )
                return MakeFormattedError<AssetRegistry>( "line {}: '{}' is neither '-' nor a 16-digit hex handle",
                                                          lineNo, std::string( identityText ) );

            if ( depsText != kNone )
            {
                std::string_view deps = depsText;
                for ( ;; )
                {
                    const std::size_t      comma      = deps.find( ',' );
                    const std::string_view one        = deps.substr( 0, comma );
                    uint64_t               dependency = 0;
                    if ( !ParseHexU64( one, dependency ) )
                        return MakeFormattedError<AssetRegistry>(
                             "line {}: '{}' in the dependency column is not a 16-digit hex handle", lineNo,
                             std::string( one ) );
                    entry.Dependencies.push_back( dependency );
                    if ( comma == std::string_view::npos )
                        break;
                    deps = deps.substr( comma + 1 );
                }
            }

            if ( boundsText != kNone )
            {
                Common::Math::AABB box;
                if ( !ParseBounds( boundsText, box ) )
                    return MakeFormattedError<AssetRegistry>(
                         "line {}: '{}' is neither '-' nor six comma-separated 8-digit hex floats making a "
                         "finite box with min <= max",
                         lineNo, std::string( boundsText ) );
                entry.Bounds = box;
            }

            if ( tagsText != kNone && !ParseTags( tagsText, entry ) )
                return MakeFormattedError<AssetRegistry>( "line {}: '{}' is neither '-' nor 'Name=<%XX-escaped "
                                                          "text>' and/or 'Skinned', comma separated",
                                                          lineNo, std::string( tagsText ) );

            if ( const auto inserted = registry.Insert( std::move( entry ) ); !inserted )
                return MakeFormattedError<AssetRegistry>( "line {}: {}", lineNo, inserted.GetError() );
        }

        return MakeSuccess( std::move( registry ) );
    }

    std::filesystem::path AssetRegistry::DefaultPath()
    {
        return Constants::Path::Dir( Constants::Path::ContentDir::Cooked ) / kFileName;
    }

    ResultStr<AssetRegistry> AssetRegistry::LoadFrom( const std::filesystem::path& path )
    {
        // THROUGH `ReadFileContent`, which is VFS-aware: in a packaged game the Cooked tree exists only
        // inside `Content.dpak`, and a bare `ifstream` here would have made the shipping host the one
        // host that cannot read its own registry.
        const auto text = FileSystem::ReadFileContent( path );
        if ( !text )
            return MakeFormattedError<AssetRegistry>( "the cooked asset registry '{}' could not be read: {}",
                                                      path.string(), text.GetError() );

        auto parsed = Parse( text.GetValue() );
        if ( !parsed )
            return MakeFormattedError<AssetRegistry>( "'{}' is not a usable cooked asset registry: {}",
                                                      path.string(), parsed.GetError() );

        return parsed;
    }
} // namespace Common::Utils
