#include "AnimGraph.hpp"

#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <rflcpp/rfl/json.hpp>
#include <rflcpp/rfl/DefaultIfMissing.hpp>

#include <array>
#include <format>

// reflect-cpp round-trip for the AnimGraph (all plain structs). DefaultIfMissing tolerates graphs saved by an
// older build that lacked a field, so adding fields never breaks existing .danimgraph files. The header is the
// one member that is NOT tolerated missing (T7d): it is the graph's identity, minted once by the migrator.
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
        return rfl::json::write( out );
    }

    Common::ResultStr<AnimGraph> Deserialize( const std::string& json )
    {
        // Generation 0 stated no version at all, so a file without a header IS version 0.
        if ( auto headed = Assets::RefuseTextWithoutHeader( json, kAnimGraphVersion, 0 ); !headed )
            return Common::MakeError<AnimGraph>( std::format( "anim graph {}", headed.GetError() ) );

        auto parsed = rfl::json::read<AnimGraph, rfl::DefaultIfMissing>( json );
        if ( !parsed )
            return Common::MakeError<AnimGraph>( std::format( "bad .danimgraph: {}", parsed.error().what() ) );

        if ( auto header = Assets::CheckStatedHeader(
                  parsed.value().Header, Common::Content::ContentKind::AnimGraph, Assets::kAnimGraphSchemaTag,
                  kAnimGraphVersion, AnimGraphTextSubsystems() );
             !header )
            return Common::MakeError<AnimGraph>( std::format( "anim graph {}", header.GetError() ) );
        return Common::MakeSuccess( parsed.value() );
    }
} // namespace Desert::Animation::Graph
