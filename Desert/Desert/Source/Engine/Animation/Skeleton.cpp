#include "Skeleton.hpp"

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <string>

namespace Desert::Animation
{
    Skeleton::Skeleton( std::vector<BoneInfo>&& bones ) : m_Bones( std::move( bones ) )
    {
        m_Signature = ComputeSignature( m_Bones );

        m_NameToIndex.reserve( m_Bones.size() );
        for ( uint32_t i = 0; i < m_Bones.size(); ++i )
        {
            // FIRST WINS, and duplicates are a structure error rather than a silent overwrite. The old linear
            // scan returned the first match too, so this preserves the behaviour every caller was written
            // against — but it now says that the rig has two bones answering to one name, which is a fact an
            // artist can act on and the scan could never report.
            if ( !m_NameToIndex.emplace( m_Bones[i].Name, i ).second )
            {
                m_StructureError += m_StructureError.empty() ? "" : " ";
                m_StructureError += fmt::format( "bone {} and bone {} are both named '{}'; a name resolves to "
                                                 "the first of them.",
                                                 m_NameToIndex[m_Bones[i].Name], i, m_Bones[i].Name );
            }
        }

        BuildStructure();
    }

    void Skeleton::BuildStructure()
    {
        const size_t n = m_Bones.size();
        m_Parents.assign( n, NO_PARENT );
        m_ResolveOrder.clear();
        m_ResolveOrder.reserve( n );

        std::vector<uint32_t> childCount( n, 0 );
        std::string           danglingParents;
        size_t                danglingCount = 0;

        for ( uint32_t i = 0; i < n; ++i )
        {
            if ( !m_Bones[i].ParentBoneID.has_value() )
                continue;

            const uint32_t parent = m_Bones[i].ParentBoneID.value();
            if ( parent >= n || parent == i )
            {
                // Resolved as a root, ONCE, here — rather than by each of the eight chain walks deciding for
                // itself, which is how one of them treated such a bone as a root and another never visited it.
                danglingParents += danglingParents.empty() ? "" : ", ";
                danglingParents += fmt::format( "{}('{}')->{}", i, m_Bones[i].Name, parent );
                ++danglingCount;
                continue;
            }

            m_Parents[i] = parent;
            ++childCount[parent];
        }

        if ( !danglingParents.empty() )
        {
            m_StructureError += m_StructureError.empty() ? "" : " ";
            m_StructureError +=
                 fmt::format( "{} bone(s) name a parent outside the array (or themselves) and resolve as "
                              "roots: [{}].",
                              danglingCount, danglingParents );
        }

        // Kahn from the roots down. What is left unvisited at the end is exactly the set of bones in a parent
        // cycle — found rather than searched for, and cheaper than the recursion that used to run off the
        // stack when one existed.
        std::vector<uint32_t> pending;
        pending.reserve( n );
        for ( uint32_t i = 0; i < n; ++i )
            if ( m_Parents[i] == NO_PARENT )
                pending.push_back( i );

        std::vector<uint8_t> visited( n, 0 );
        for ( size_t head = 0; head < pending.size(); ++head )
        {
            const uint32_t bone = pending[head];
            visited[bone]       = 1;
            m_ResolveOrder.push_back( bone );
            if ( childCount[bone] == 0 )
                continue;
            for ( uint32_t child = 0; child < n; ++child )
                if ( m_Parents[child] == bone )
                    pending.push_back( child );
        }

        if ( m_ResolveOrder.size() != n )
        {
            std::string cyclic;
            size_t      cyclicCount = 0;
            for ( uint32_t i = 0; i < n; ++i )
                if ( !visited[i] )
                {
                    ++cyclicCount;
                    // Cut the cycle: these bones become roots, so a resolve terminates and produces a defined
                    // (wrong, and reported) pose instead of exhausting the stack.
                    m_Parents[i] = NO_PARENT;
                    m_ResolveOrder.push_back( i );
                    cyclic += cyclic.empty() ? "" : ", ";
                    cyclic += fmt::format( "{}('{}')", i, m_Bones[i].Name );
                }
            m_StructureError += m_StructureError.empty() ? "" : " ";
            m_StructureError += fmt::format( "{} bone(s) form a parent cycle and are resolved as roots: [{}].",
                                             cyclicCount, cyclic );
        }
    }

    bool Skeleton::SetLocalBindTransform( uint32_t bone, const glm::mat4& localBind )
    {
        if ( bone >= m_Bones.size() )
            return false;
        m_Bones[bone].LocalBindTransform = localBind;
        return true;
    }

    void Skeleton::WriteBindSkinningMatrices( std::vector<glm::mat4>& out ) const
    {
        std::vector<glm::mat4> global;
        ResolveComponentSpace( [this]( uint32_t i ) { return m_Bones[i].LocalBindTransform; }, global );

        out.resize( m_Bones.size() );
        for ( size_t i = 0; i < m_Bones.size(); ++i )
            out[i] = global[i] * m_Bones[i].OffsetMatrix;
    }

    void Skeleton::RecomputeOffsetMatrices()
    {
        std::vector<glm::mat4> global;
        ResolveComponentSpace( [this]( uint32_t i ) { return m_Bones[i].LocalBindTransform; }, global );
        for ( size_t i = 0; i < m_Bones.size(); ++i )
            m_Bones[i].OffsetMatrix = glm::inverse( global[i] );
    }

    Common::BoolResultStr Skeleton::ValidateBoneIndexSpace( const std::vector<uint32_t>& boneIDs,
                                                            const std::string&           sourceName ) const
    {
        const uint32_t boneCount = static_cast<uint32_t>( m_Bones.size() );

        uint32_t worst = 0;
        size_t   bad   = 0;
        for ( const uint32_t id : boneIDs )
            if ( id >= boneCount )
            {
                ++bad;
                worst = std::max( worst, id );
            }

        if ( bad == 0 )
            return Common::MakeSuccess( true );

        return Common::MakeFormattedError<bool>(
             "'{}' skins {} vertex influence(s) to a bone index this rig does not have (highest {}, the rig "
             "has {} bones). A mesh's BoneIDs, a skeleton's bone index and a pose's index are ONE index "
             "space in this engine — that is why there is no linkup table — so a mesh cooked against a "
             "different bone order reads past the end of the pose instead of being remapped.",
             sourceName, bad, worst, boneCount );
    }

    // ORDER-INDEPENDENT signature: identifies a rig by its SET of (bone name -> parent bone name) edges, NOT by
    // the order bones happen to appear in the array. This matters because the same Mixamo rig exported WITH a
    // skin (character) vs WITHOUT a skin (animation file) yields the bones in different array orders / from
    // different sources (mesh weights vs animation channels) — an order-sensitive hash would give them
    // different signatures and the animation would never match the character. Hashing a sorted set of
    // name<parentName entries makes both produce the SAME signature while still distinguishing different rigs.
    uint64_t Skeleton::ComputeSignature( const std::vector<BoneInfo>& bones )
    {
        std::vector<std::string> entries;
        entries.reserve( bones.size() );
        for ( const auto& bone : bones )
        {
            const std::string parentName =
                 ( bone.ParentBoneID.has_value() && bone.ParentBoneID.value() < bones.size() )
                      ? bones[bone.ParentBoneID.value()].Name
                      : std::string();
            entries.push_back( bone.Name + '<' + parentName );
        }
        std::sort( entries.begin(), entries.end() );

        uint64_t hash = 1469598103934665603ULL;
        for ( const auto& entry : entries )
            for ( char c : entry )
                hash = ( hash ^ static_cast<unsigned char>( c ) ) * 1099511628211ULL;

        return hash;
    }
} // namespace Desert::Animation
