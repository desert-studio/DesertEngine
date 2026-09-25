#include "AnimGraph.hpp"

#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Json/Json.hpp>

#include <array>
#include <format>

// Common::Json round-trip for the AnimGraph (all plain structs), read STRICTLY: a field missing from a
// .danimgraph is an error naming its path, never a default filled in behind the reader's back. A field added
// later is either std::optional (its absence means something) or moved into the files by a migration. The
// header is checked first on its own (T7d): it is the graph's identity, minted once by the migrator.
namespace Desert::Animation::Graph
{
    namespace
    {
        constexpr int kAnimGraphVersion = static_cast<int>( Assets::kAnimGraphSchemaVersion );

        std::span<const Common::Content::SubsystemVersion> AnimGraphTextSubsystems()
        {
            static const std::array<Common::Content::SubsystemVersion, 1> versions = {
                 Common::Content::SubsystemVersion{ Assets::kAnimGraphSchemaTag,
                                                    Assets::kAnimGraphSchemaVersion } };
            return versions;
        }
    } // namespace

    std::string Serialize( const AnimGraph& graph )
    {
        AnimGraph out = graph;
        out.Header    = Assets::StampTextHeader( graph.Header, Common::Content::ContentKind::AnimGraph,
                                                 AnimGraphTextSubsystems() );
        return Common::Json::Write( out );
    }

    Common::ResultStr<AnimGraph> Deserialize( const std::string& json )
    {
        // Generation 0 stated no version at all, so a file without a header IS version 0.
        if ( auto headed = Assets::RefuseTextWithoutHeader( json, kAnimGraphVersion, 0 ); !headed )
            return Common::MakeError<AnimGraph>( std::format( "anim graph {}", headed.GetError() ) );

        auto parsed = Common::Json::Read<AnimGraph>( json );
        if ( !parsed )
            return Common::MakeError<AnimGraph>( std::format( "bad .danimgraph: {}", parsed.GetError() ) );

        if ( auto header = Assets::CheckStatedHeader(
                  parsed.GetValue().Header, Common::Content::ContentKind::AnimGraph, Assets::kAnimGraphSchemaTag,
                  kAnimGraphVersion, AnimGraphTextSubsystems() );
             !header )
            return Common::MakeError<AnimGraph>( std::format( "anim graph {}", header.GetError() ) );
        return Common::MakeSuccess( parsed.GetValue() );
    }
} // namespace Desert::Animation::Graph
