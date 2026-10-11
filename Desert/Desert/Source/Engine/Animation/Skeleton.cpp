#include "Skeleton.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <string>
#include <unordered_set>

namespace Desert::Animation
{
    namespace
    {
        // See Skeleton::GetAuthoringRevision: unique across the process, never 0.
        uint64_t NextAuthoringRevision()
        {
            static std::atomic<uint64_t> next{ 0 };
            return ++next;
        }
    } // namespace

    Skeleton::Skeleton( std::vector<BoneInfo>&& bones )
         : m_Bones( std::move( bones ) ), m_Signature( ComputeSignature( m_Bones ) ),
           m_ContentSignature( ComputeContentSignature( m_Bones ) )
    {
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
        m_AuthoringRevision = NextAuthoringRevision();
    }

    void Skeleton::BuildStructure()
    {
        const size_t count = m_Bones.size();
        m_Parents.assign( count, NO_PARENT );
        m_ResolveOrder.clear();
        m_ResolveOrder.reserve( count );

        std::vector<uint32_t> childCount( count, 0 );
        std::string           danglingParents;
        size_t                danglingCount = 0;

        for ( uint32_t i = 0; i < count; ++i )
        {
            if ( !m_Bones[i].ParentBoneID.has_value() )
            {
                continue;
            }

            const uint32_t parent = m_Bones[i].ParentBoneID.value();
            if ( parent >= count || parent == i )
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
        pending.reserve( count );
        for ( uint32_t i = 0; i < count; ++i )
            if ( m_Parents[i] == NO_PARENT )
                pending.push_back( i );

        std::vector<uint8_t> visited( count, 0 );
        for ( size_t head = 0; head < pending.size(); ++head )
        {
            const uint32_t bone = pending[head];
            visited[bone]       = 1;
            m_ResolveOrder.push_back( bone );
            if ( childCount[bone] == 0 )
            {
                continue;
            }
            for ( uint32_t child = 0; child < count; ++child )
                if ( m_Parents[child] == bone )
                    pending.push_back( child );
        }

        if ( m_ResolveOrder.size() != count )
        {
            std::string cyclic;
            size_t      cyclicCount = 0;
            for ( uint32_t i = 0; i < count; ++i )
            {
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
            }
            m_StructureError += m_StructureError.empty() ? "" : " ";
            m_StructureError += fmt::format( "{} bone(s) form a parent cycle and are resolved as roots: [{}].",
                                             cyclicCount, cyclic );
        }

        // The inverse of the order, built here because this is the one place that knows the order is total:
        // every bone reaches m_ResolveOrder exactly once — through Kahn from the roots, or through the
        // cycle-cutting pass above that makes the leftovers roots. See GetResolveRank for what needs it.
        m_ResolveRank.assign( count, 0 );
        for ( uint32_t rank = 0; rank < m_ResolveOrder.size(); ++rank )
        {
            m_ResolveRank[m_ResolveOrder[rank]] = rank;
        }
    }

    bool Skeleton::SetLocalBindTransform( uint32_t bone, const glm::mat4& localBind )
    {
        const bool inRange = bone < m_Bones.size();
        if ( inRange )
        {
            m_Bones[bone].LocalBindTransform = localBind;
        }
        return inRange;
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
        {
            return Common::MakeSuccess( true );
        }

        return Common::MakeFormattedError<bool>(
             "'{}' skins {} vertex influence(s) to a bone index this rig does not have (highest {}, the rig "
             "has {} bones). A mesh's BoneIDs, a skeleton's bone index and a pose's index are ONE index "
             "space in this engine — that is why there is no linkup table — so a mesh cooked against a "
             "different bone order reads past the end of the pose instead of being remapped.",
             sourceName, bad, worst, boneCount );
    }

    // ORDER-INDEPENDENT signature: identifies a rig by its SET of (bone name -> parent bone name) edges, NOT
    // by the order bones happen to appear in the array. This matters because the same Mixamo rig exported WITH
    // a skin (character) vs WITHOUT a skin (animation file) yields the bones in different array orders / from
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

    uint64_t Skeleton::ComputeContentSignature( const std::vector<BoneInfo>& bones )
    {
        uint64_t   hash = ComputeSignature( bones );
        const auto fold = [&hash]( const glm::mat4& m )
        {
            for ( const unsigned char byte : std::bit_cast<std::array<unsigned char, sizeof( glm::mat4 )>>( m ) )
                hash = ( hash ^ byte ) * 1099511628211ULL;
        };
        for ( const auto& bone : bones )
        {
            fold( bone.LocalBindTransform );
            fold( bone.OffsetMatrix );
        }
        return hash;
    }

    Common::BoolResultStr Skeleton::SetSockets( std::vector<SkeletonSocket> sockets )
    {
        std::unordered_set<std::string> names;
        for ( const SkeletonSocket& socket : sockets )
        {
            if ( socket.Name.empty() )
                return Common::MakeFormattedError<bool>( "a socket on bone '{}' has no name", socket.Bone );
            if ( !names.insert( socket.Name ).second )
                return Common::MakeFormattedError<bool>( "two sockets are named '{}'", socket.Name );
            if ( !FindBoneIndex( socket.Bone ) )
                return Common::MakeFormattedError<bool>(
                     "socket '{}' is on bone '{}', which the skeleton does not have", socket.Name, socket.Bone );
        }
        m_Sockets           = std::move( sockets );
        m_AuthoringRevision = NextAuthoringRevision();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr Skeleton::SetBoneMasks( std::vector<BoneMask> masks )
    {
        std::unordered_set<std::string> names;
        for ( const BoneMask& mask : masks )
        {
            if ( mask.Name.empty() )
                return Common::MakeError<bool>( "a bone mask has no name" );
            if ( !names.insert( mask.Name ).second )
                return Common::MakeFormattedError<bool>( "two bone masks are named '{}'", mask.Name );
            for ( const BoneMaskEntry& entry : mask.Entries )
                if ( !( entry.Weight >= 0.0F && entry.Weight <= 1.0F ) )
                    return Common::MakeFormattedError<bool>(
                         "bone mask '{}' weighs bone '{}' at {}, outside [0, 1]", mask.Name, entry.Bone,
                         entry.Weight );
            if ( auto resolved = ResolveBoneMaskWeights( mask, *this ); !resolved )
                return Common::MakeError<bool>( resolved.GetError() );
        }
        m_BoneMasks         = std::move( masks );
        m_AuthoringRevision = NextAuthoringRevision();
        return BOOLSUCCESS;
    }

    const SkeletonSocket* Skeleton::FindSocket( const std::string_view name ) const
    {
        const auto it = std::ranges::find( m_Sockets, name, &SkeletonSocket::Name );
        return it == m_Sockets.end() ? nullptr : &*it;
    }

    const BoneMask* Skeleton::FindBoneMask( const std::string_view name ) const
    {
        const auto it = std::ranges::find( m_BoneMasks, name, &BoneMask::Name );
        return it == m_BoneMasks.end() ? nullptr : &*it;
    }

    Common::ResultStr<std::vector<float>> ResolveBoneMaskWeights( const BoneMask& mask, const Skeleton& skeleton )
    {
        using Weights          = std::vector<float>;
        const size_t boneCount = skeleton.GetBones().size();
        // Per bone: the entry that names it, if any.
        std::vector<const BoneMaskEntry*> entryOf( boneCount, nullptr );
        for ( const BoneMaskEntry& entry : mask.Entries )
        {
            const auto bone = skeleton.FindBoneIndex( entry.Bone );
            if ( !bone )
                return Common::MakeError<Weights>(
                     fmt::format( "bone mask '{}' weighs bone '{}', which the skeleton does not have", mask.Name,
                                  entry.Bone ) );
            if ( entryOf[*bone] != nullptr )
                return Common::MakeError<Weights>(
                     fmt::format( "bone mask '{}' weighs bone '{}' twice", mask.Name, entry.Bone ) );
            entryOf[*bone] = &entry;
        }

        Weights weight( boneCount, 0.0F );
        Weights handedDown( boneCount, 0.0F ); // what a bone's children inherit
        for ( const uint32_t b : skeleton.GetResolveOrder() )
        {
            const uint32_t parent    = skeleton.ResolveParent( b );
            const float    inherited = parent == Skeleton::NO_PARENT ? 0.0F : handedDown[parent];
            if ( const BoneMaskEntry* entry = entryOf[b] )
            {
                weight[b]     = entry->Weight;
                handedDown[b] = entry->IncludeDescendants ? entry->Weight : inherited;
            }
            else
            {
                weight[b]     = inherited;
                handedDown[b] = inherited;
            }
        }
        return Common::MakeSuccess( std::move( weight ) );
    }
} // namespace Desert::Animation
