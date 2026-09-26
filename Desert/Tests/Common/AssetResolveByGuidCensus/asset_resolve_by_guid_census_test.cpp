// A MOVED ASSET IS STILL THE SAME ASSET — a census over every content kind (AF10a).
//
// The relation asserted, per kind:
//
//     Move an asset of this kind to another directory of a temporary content root, leave every
//     referrer untouched, and put a DIFFERENT asset of the same kind where the moved one used to be.
//     The reference a referrer wrote before the move must still name the MOVED asset.
//
// This is UE's soft-reference rule (FSoftObjectPath resolves through the Asset Registry by identity;
// the path is a locator, and a stale locator is what UObjectRedirector exists for). Here identity is
// the header GUID, and the ONE resolver that decides "GUID first, the path only as a hint" is
// `AssetRegistry::FindByGuidReference`. The decoy at the old path is what makes the census able to
// tell "resolved by GUID" from "resolved by path": without it a path-first resolver and a GUID-first
// one would both answer after the move (the path misses, the GUID hits), and the mutation "resolve by
// path first" would stay green.
//
// WHY A REGISTER OF NAMED ROWS AND NOT A COUNT. A kind that still resolves only by path is not a
// failure of this suite — it is work not done yet, and each such kind is ONE row below with the reason
// and the task that closes it. The census is exact in BOTH directions: a kind outside the register
// that fails is red, and a kind INSIDE the register that now resolves by GUID is red too, so the
// register cannot outlive the defect it names.
//
// FIXTURES ARE THE SHIPPED FILES where the corpus has one (`Editor/Resources/Assets`), because the
// question "does this format state a GUID" is a question about the real files, not about what a
// fixture writer can produce. Only kinds with no committed sample are synthesised (meshes are imported
// into Cooked/ and never committed; skeletons, animations, shaders and world cells likewise), and the
// synthetic file is the smallest the kind's own header format accepts.
//
// WHAT IS NOT CLAIMED. The census models each referrer by the spelling it writes (a GUID beside the
// path, or the path alone — kGuidReferrers); it does not load a scene through the engine's
// AssetManager, which a Common-only suite cannot link.

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/AssetRedirector.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Common::Content::AssetGuid;
using Common::Content::ContentKind;

namespace
{
    // ── THE REGISTER ────────────────────────────────────────────────────────────────────────────
    // Every kind a referrer still reaches by PATH, with why and what closes it. One row per kind; the
    // census derives the set from here, so closing a defect means deleting its row.
    struct PathOnlyRow
    {
        ContentKind      Kind;
        std::string_view Reason;
    };

    constexpr std::array kPathOnly = {
         PathOnlyRow{ ContentKind::Skeleton, "no header format: a .skeleton states no GUID (AF4 mesh programme)" },
         PathOnlyRow{ ContentKind::Animation, "no header format: an .anim states no GUID (AF4 mesh programme)" },
         PathOnlyRow{ ContentKind::CloudNoiseVolume, "raw DCNV file, no DAST envelope: states no GUID (AF10e)" },
         PathOnlyRow{ ContentKind::CloudModellingVolume,
                      "raw DCMV file, no DAST envelope; StoredFormFor(CloudModellingVolumeAsset) = AssetsRelative "
                      "path (AF10e)" },
         PathOnlyRow{
              ContentKind::UITheme,
              "header states a GUID but StoredFormFor(UIThemeAsset) writes AssetsRelative path only (AF10f)" },
         PathOnlyRow{
              ContentKind::ControlRig,
              "header states a GUID but StoredFormFor(ControlRigAsset) writes AssetsRelative path only (AF10f)" },
         PathOnlyRow{
              ContentKind::AnimGraph,
              "header states a GUID but StoredFormFor(AnimGraphAsset) writes AssetsRelative path only (AF10f)" },
         PathOnlyRow{
              ContentKind::Retarget,
              "header states a GUID but StoredFormFor(RetargetAsset) writes AssetsRelative path only (AF10f)" },
         PathOnlyRow{ ContentKind::Prefab,
                      "header states a GUID but PrefabComponent writes PrefabPath only (PrefabData.hpp) (AF10f)" },
         PathOnlyRow{ ContentKind::WorldCell,
                      "envelope states a GUID but the index names cells by file name (AF10f, with WP)" },
         PathOnlyRow{
              ContentKind::WorldIndex,
              "envelope states a GUID but a world is found by its directory beside the scene (AF10f, with WP)" },
    };

    // THE KINDS A REDIRECTOR CARRIES (AF10b): their referrers still write the path alone, but the file a move
    // leaves at the old path (Common/Content/AssetRedirector.hpp) names the moved asset's GUID, so the
    // pre-move reference reaches it. Proven by the census with a redirector at the old path instead of a decoy.
    constexpr std::array kRedirected = { ContentKind::StringTable, ContentKind::Scene };

    // The kinds whose referrers write a GUID beside the path, each with the reader that honours it:
    // mesh/material slots of mesh components and the texture slot (ResolveGuidRef, ComponentRegistry.cpp
    // FromGuid branches), skyboxes (SCNE 29), and a .demat's texture/cloud references (MaterialAssetRef,
    // MaterialData.hpp), and shaders - a .demat's Shader (MATL 4) and a scene's Material.Shader (SCNE 31),
    // both {Guid, Path} against the `.shader` comment header the registry reads (ShaderCommentHeaderFormat).
    // Every other kind's referrer writes the path alone.
    constexpr std::array kGuidReferrers = {
         ContentKind::StaticMesh, ContentKind::SkinnedMesh, ContentKind::Texture,     ContentKind::Material,
         ContentKind::Skybox,     ContentKind::CloudType,   ContentKind::CloudLayout, ContentKind::Shader };

    template <class Array>
    bool Contains( const Array& kinds, ContentKind kind )
    {
        return std::ranges::any_of( kinds, [kind]( const ContentKind k ) { return k == kind; } );
    }

    std::optional<std::string_view> PathOnlyReason( ContentKind kind )
    {
        for ( const PathOnlyRow& row : kPathOnly )
            if ( row.Kind == kind )
                return row.Reason;
        return std::nullopt;
    }

    fs::path RepoRoot()
    {
        return fs::path( __FILE__ ).parent_path().parent_path().parent_path().parent_path().parent_path();
    }

    std::vector<char> ReadBytes( const fs::path& file )
    {
        std::ifstream in( file, std::ios::binary );
        return { std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() };
    }

    void WriteBytes( const fs::path& file, const std::vector<char>& bytes )
    {
        fs::create_directories( file.parent_path() );
        std::ofstream out( file, std::ios::binary | std::ios::trunc );
        out.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
    }

    // The committed sample of `kind`, found by extension under the kind's directory of the corpus.
    std::optional<fs::path> CorpusSample( ContentKind kind )
    {
        const fs::path  corpus = RepoRoot() / "Editor/Resources/Assets";
        const auto&     spec   = Common::Content::KindSpec( kind );
        std::error_code ec;
        for ( auto it = fs::recursive_directory_iterator( corpus, ec );
              !ec && it != fs::recursive_directory_iterator(); it.increment( ec ) )
        {
            if ( !it->is_regular_file() || it->path().extension() != spec.Extension )
                continue;
            // Skyboxes and textures share `.detex` and are told apart by root; the corpus keeps sky
            // textures under Textures/HDR.
            const bool underSky = it->path().string().find( "HDR" ) != std::string::npos;
            if ( kind == ContentKind::Texture && underSky )
                continue;
            if ( kind == ContentKind::Skybox && !underSky )
                continue;
            return it->path();
        }
        return std::nullopt;
    }

    std::vector<char> SyntheticMesh( bool skinned, const AssetGuid& guid )
    {
        Common::Content::MeshBinaryFileHeader header{};
        std::copy_n( Common::Content::kMeshBinaryMagic, sizeof( header.Magic ), header.Magic );
        header.ByteOrder = Common::Content::kMeshBinaryByteOrderTag;
        header.Version   = Common::Content::kMeshBinaryVersion;
        constexpr std::size_t kTableEnd =
             Common::Content::kMeshBinaryPrefixV3 + Common::Content::kMeshBinarySectionRowSize;
        header.FileSize     = kTableEnd;
        header.SectionCount = 1;
        header.Flags        = skinned ? Common::Content::kMeshFlagIsSkinned : 0u;
        std::vector<char> bytes( kTableEnd );
        // One section row: the submesh table, empty. The header format requires the table to exist.
        const uint32_t id          = Common::Content::kMeshBinarySubmeshSectionId;
        const uint32_t elementSize = Common::Content::kMeshBinarySubmeshSizeV3;
        const uint64_t offset      = kTableEnd;
        const uint64_t count       = 0;
        const auto     rowAt       = bytes.begin() + Common::Content::kMeshBinaryPrefixV3;
        const auto     idBytes     = std::bit_cast<std::array<char, 4>>( id );
        const auto     sizeBytes   = std::bit_cast<std::array<char, 4>>( elementSize );
        const auto     offsetBytes = std::bit_cast<std::array<char, 8>>( offset );
        const auto     countBytes  = std::bit_cast<std::array<char, 8>>( count );
        std::copy( idBytes.begin(), idBytes.end(), rowAt );
        std::copy( sizeBytes.begin(), sizeBytes.end(), rowAt + 4 );
        std::copy( offsetBytes.begin(), offsetBytes.end(), rowAt + 8 );
        std::copy( countBytes.begin(), countBytes.end(), rowAt + 16 );
        const auto headerBytes = std::bit_cast<std::array<char, sizeof( header )>>( header );
        std::copy( headerBytes.begin(), headerBytes.end(), bytes.begin() );
        std::memcpy( bytes.data() + Common::Content::kMeshBinaryGuidOffset, &guid.Hi, 8 );
        std::memcpy( bytes.data() + Common::Content::kMeshBinaryGuidOffset + 8, &guid.Lo, 8 );
        return bytes;
    }

    std::vector<char> SyntheticEnvelope( ContentKind kind, const AssetGuid& guid )
    {
        Common::Content::AssetEnvelope envelope;
        envelope.Asset.Kind = kind;
        envelope.Asset.Guid = guid;
        auto bytes          = Common::Content::WriteAssetEnvelope( envelope );
        if ( !bytes )
            return {};
        std::vector<char> out( bytes.GetValue().size() );
        std::memcpy( out.data(), bytes.GetValue().data(), out.size() );
        return out;
    }

    // The fixture's bytes: the corpus sample, else the smallest file the kind's own format accepts.
    std::vector<char> FixtureBytes( ContentKind kind )
    {
        if ( const auto sample = CorpusSample( kind ) )
            return ReadBytes( *sample );
        const AssetGuid guid{ 0xAF10A000ull + static_cast<uint64_t>( kind ), 0x5EEDull };
        switch ( kind )
        {
            case ContentKind::Redirector:
                return {};
            case ContentKind::StaticMesh:
                return SyntheticMesh( false, guid );
            case ContentKind::SkinnedMesh:
                return SyntheticMesh( true, guid );
            case ContentKind::WorldCell:
            case ContentKind::WorldIndex:
            case ContentKind::Skybox:
                return SyntheticEnvelope( kind, guid );
            case ContentKind::Shader:
            {
                // The `.shader` header is its first line's comment (T7j); the body is the least the declared-name
                // check accepts, named after the probe file.
                const std::string text =
                     Common::Content::WriteShaderHeaderLine( Common::Content::MakeTextHeader( kind, guid, {} ) ) +
                     "Shader \"AF10a_Probe\" {}\n";
                return { text.begin(), text.end() };
            }
            default:
                // Skeleton, animation: formats with no header at all.
                return { 'N', 'O', 'H', 'D', 'R' };
        }
    }

    // The same asset with a DIFFERENT identity: the decoy that takes the vacated path.
    std::vector<char> WithOtherGuid( std::vector<char> bytes, const std::optional<AssetGuid>& guid )
    {
        if ( !guid )
            return bytes; // no identity to change: the decoy is a byte copy
        const AssetGuid        other{ ~guid->Hi, ~guid->Lo };
        const std::string_view view( bytes.data(), bytes.size() );
        if ( view.starts_with( "DESTMESH" ) )
        {
            std::memcpy( bytes.data() + Common::Content::kMeshBinaryGuidOffset, &other.Hi, 8 );
            std::memcpy( bytes.data() + Common::Content::kMeshBinaryGuidOffset + 8, &other.Lo, 8 );
            return bytes;
        }
        if ( view.starts_with( "DAST" ) )
        {
            std::vector<std::byte> raw( bytes.size() );
            std::memcpy( raw.data(), bytes.data(), bytes.size() );
            auto envelope = Common::Content::ReadAssetEnvelope( raw, { {}, true } );
            if ( !envelope )
                return {};
            Common::Content::AssetEnvelope copy = envelope.GetValue();
            copy.Asset.Guid                     = other;
            auto rewritten                      = Common::Content::WriteAssetEnvelope( copy );
            if ( !rewritten )
                return {};
            std::vector<char> out( rewritten.GetValue().size() );
            std::memcpy( out.data(), rewritten.GetValue().data(), out.size() );
            return out;
        }
        std::string       text( view );
        const std::string from = Common::Content::AssetGuidToText( *guid );
        const auto        at   = text.find( from );
        if ( at == std::string::npos )
            return {};
        text.replace( at, from.size(), Common::Content::AssetGuidToText( other ) );
        return { text.begin(), text.end() };
    }

    class ProjectRootGuard
    {
    public:
        ProjectRootGuard() : m_Saved( Common::Constants::Path::CurrentProjectRoot() )
        {
        }
        ~ProjectRootGuard()
        {
            Common::Constants::Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
        }
        ProjectRootGuard( const ProjectRootGuard& )            = delete;
        ProjectRootGuard& operator=( const ProjectRootGuard& ) = delete;

    private:
        Common::Constants::Path::ProjectRootState m_Saved;
    };

    struct Outcome
    {
        std::string Error; // non-empty: the fixture itself could not be set up
        bool        ResolvedToMoved = false;
        std::string Answer; // the key the reference resolved to, or "(nothing)"
    };

    // Moves one asset of `kind` inside a fresh project and resolves the reference a referrer wrote before.
    Outcome MoveAndResolve( ContentKind kind, const fs::path& project, bool leaveRedirector )
    {
        Outcome                 outcome;
        const std::vector<char> bytes  = FixtureBytes( kind );
        const auto&             spec   = Common::Content::KindSpec( kind );
        const fs::path          name   = fs::path( "AF10a_Probe" ).replace_extension( spec.Extension );
        const fs::path          root   = spec.Root->is_absolute() ? *spec.Root : project / *spec.Root;
        const fs::path          before = root / "AF10a_Before" / name;
        const fs::path          after  = root / "AF10a_After" / name;
        WriteBytes( before, bytes );

        const auto described = Common::Content::DescribeContentFile( before, kind );
        const auto original =
             Common::Content::RegistryRowFor( Common::AssetHandle::StableKeyForPath( before ), described );
        if ( !original )
        {
            outcome.Error = std::string( original.GetError() );
            return outcome;
        }
        // What the referrer wrote: the GUID beside the path where its format has one, the path alone otherwise.
        const std::optional<AssetGuid>& stated = original.GetValue().Guid;
        const AssetGuid written = Contains( kGuidReferrers, kind ) ? stated.value_or( AssetGuid{} ) : AssetGuid{};

        fs::create_directories( after.parent_path() );
        fs::rename( before, after );
        if ( leaveRedirector )
        {
            const Common::Content::AssetRedirector redirector{ AssetGuid::Generate(),
                                                               stated.value_or( AssetGuid{} ),
                                                               Common::AssetHandle::StableKeyForPath( before ) };
            if ( auto written = Common::Content::WriteRedirectorFile( before, redirector ); !written )
            {
                outcome.Error = std::string( written.GetError() );
                return outcome;
            }
        }
        else
        {
            const std::vector<char> decoy = WithOtherGuid( bytes, original.GetValue().Guid );
            if ( decoy.empty() )
            {
                outcome.Error = "could not build a decoy with another GUID";
                return outcome;
            }
            WriteBytes( before, decoy );
        }

        Common::Utils::AssetRegistry registry;
        for ( const fs::path& file : { after, before } )
        {
            auto row = Common::Content::RegistryRowFor( Common::AssetHandle::StableKeyForPath( file ),
                                                        Common::Content::DescribeContentFile( file, kind ) );
            if ( !row )
            {
                outcome.Error = std::string( row.GetError() );
                return outcome;
            }
            if ( auto inserted = registry.Insert( row.GetValue() ); !inserted )
            {
                outcome.Error = std::string( inserted.GetError() );
                return outcome;
            }
        }

        const auto* resolved = registry.FindByGuidReference( written, before.string() );
        outcome.Answer       = resolved != nullptr ? resolved->Key : std::string( "(nothing)" );
        outcome.ResolvedToMoved =
             resolved != nullptr && resolved->Key == Common::AssetHandle::StableKeyForPath( after );
        return outcome;
    }
} // namespace

TEST( AssetResolveByGuidCensus, EveryKindSurvivesAMoveOrIsANamedRegisterRow )
{
    const ProjectRootGuard guard;
    const fs::path         project = fs::temp_directory_path() / "AF10a_ResolveByGuid";
    fs::remove_all( project );
    fs::create_directories( project );
    Common::Constants::Path::SetProjectRoot( fs::canonical( project ), "Resources/Assets" );

    for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
    {
        const auto        kind = static_cast<ContentKind>( i );
        const std::string name( Common::Content::KindSpec( kind ).Name );
        if ( Common::Content::KindSpec( kind ).StatedOnly() ) // a redirector is the mechanism, not a referent
            continue;
        const bool    redirected = Contains( kRedirected, kind );
        const Outcome result     = MoveAndResolve( kind, fs::canonical( project ), redirected );
        const auto    reason     = PathOnlyReason( kind );
        if ( !result.Error.empty() )
        {
            ADD_FAILURE() << name << ": fixture could not be moved and rescanned: " << result.Error;
            continue;
        }
        if ( reason )
            EXPECT_FALSE( result.ResolvedToMoved )
                 << name << " now resolves by GUID after a move — delete its register row (\"" << *reason << "\")";
        else
            EXPECT_TRUE( result.ResolvedToMoved )
                 << name << ": after the move the pre-move reference resolved to " << result.Answer
                 << " instead of the moved asset — it resolves by PATH"
                 << ( redirected ? " even through the redirector left at the old path" : "" )
                 << ". Fix it or add a named register row.";
    }
    fs::remove_all( project );
}

// Every register row names a real kind once: a duplicate would let one deletion leave the kind listed.
TEST( AssetResolveByGuidCensus, RegisterRowsAreUniqueAndCarryAReason )
{
    for ( std::size_t i = 0; i < kPathOnly.size(); ++i )
    {
        EXPECT_FALSE( kPathOnly[i].Reason.empty() );
        for ( std::size_t j = i + 1; j < kPathOnly.size(); ++j )
            EXPECT_NE( kPathOnly[i].Kind, kPathOnly[j].Kind ) << "row " << i << " and row " << j;
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
