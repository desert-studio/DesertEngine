#include "InstanceFold.hpp"

#include <sstream>

namespace Desert::Editor::Commands
{
    bool FoldMeshIdentity::SameAs( const FoldMeshIdentity& other ) const
    {
        // THE MESH, THE MATERIALS AND THE SHADOW FLAG ARE ALL PART OF THE IDENTITY, and the last one is
        // the easiest to forget: an ISM carries ONE CastShadows for every instance it holds, so folding
        // a shadow-casting prop together with one whose shadow was deliberately turned off would put a
        // shadow back that somebody had removed — a visible change that no message announced.
        if ( Mesh != other.Mesh )
            return false;
        if ( Primitive.has_value() != other.Primitive.has_value() )
            return false;
        if ( Primitive.has_value() && *Primitive != *other.Primitive )
            return false;
        if ( CastShadows != other.CastShadows )
            return false;
        return Materials == other.Materials;
    }

    std::string FoldMeshIdentity::Describe() const
    {
        std::ostringstream out;
        if ( Primitive.has_value() )
            out << "the " << Desert::Geometry::PrimitiveTypeName( *Primitive ) << " primitive";
        else if ( !Mesh.IsNull() )
            out << "mesh " << static_cast<uint64_t>( Mesh );
        else
            out << "no mesh";

        out << " with " << Materials.size() << ( Materials.size() == 1 ? " material slot" : " material slots" );
        if ( !CastShadows )
            out << ", shadows off";
        return out.str();
    }

    Common::ResultStr<FoldPlan> PlanInstanceFold( const std::vector<FoldCandidate>& candidates )
    {
        // ONE is not a refusal for being pointless — it is a refusal because the caller's selection did
        // not contain what it thought it did, and saying "2 of the 7 selected entities carry a static
        // mesh" is the difference between a person fixing their selection and a person retrying it.
        if ( candidates.size() < 2 )
        {
            std::ostringstream out;
            out << "Collapse needs at least two static meshes that match; the selection offers "
                << candidates.size() << ".";
            return Common::MakeError<FoldPlan>( out.str() );
        }

        // Blockers first: a mixed-identity message is useless to somebody whose real problem is that one
        // of the props has a collider on it.
        std::ostringstream blocked;
        size_t             blockedCount = 0;
        for ( const FoldCandidate& candidate : candidates )
        {
            if ( candidate.Blockers.empty() )
                continue;
            if ( blockedCount++ > 0 )
                blocked << "; ";
            blocked << candidate.Name << " carries ";
            for ( size_t i = 0; i < candidate.Blockers.size(); ++i )
                blocked << ( i > 0 ? ", " : "" ) << candidate.Blockers[i];
        }
        if ( blockedCount > 0 )
        {
            std::ostringstream out;
            out << "Collapse would destroy what an instanced mesh cannot carry (" << blockedCount << " of "
                << candidates.size() << " selected): " << blocked.str() << ".";
            return Common::MakeError<FoldPlan>( out.str() );
        }

        // One identity, or none. Counting the distinct ones rather than stopping at the first mismatch
        // means the message can say HOW MANY groups the selection is, which tells the person whether
        // they picked one prop too many or two different props entirely.
        std::vector<FoldMeshIdentity> distinct;
        for ( const FoldCandidate& candidate : candidates )
        {
            bool seen = false;
            for ( const FoldMeshIdentity& known : distinct )
                seen = seen || known.SameAs( candidate.Identity );
            if ( !seen )
                distinct.push_back( candidate.Identity );
        }
        if ( distinct.size() != 1 )
        {
            std::ostringstream out;
            out << "Collapse needs one mesh, one material set and one shadow flag; the selection holds "
                << distinct.size() << " different ones (";
            for ( size_t i = 0; i < distinct.size(); ++i )
                out << ( i > 0 ? ", " : "" ) << distinct[i].Describe();
            out << ").";
            return Common::MakeError<FoldPlan>( out.str() );
        }

        FoldPlan plan;
        plan.Identity = distinct.front();
        plan.Sources.reserve( candidates.size() );
        plan.InstanceTransforms.reserve( candidates.size() );
        for ( const FoldCandidate& candidate : candidates )
        {
            plan.Sources.push_back( candidate.Entity );
            plan.InstanceTransforms.push_back( candidate.World );
        }
        return Common::MakeSuccess( std::move( plan ) );
    }
} // namespace Desert::Editor::Commands
