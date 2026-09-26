// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/DynamicMeshTriangleAttribute.h:1-692,
// adapted: UE Core as std/glm, namespace Desert::Geometry, TUniquePtr is std::unique_ptr; undo/redo change
// objects, FArchive serialization and the std::string label attribute not ported.
#pragma once


#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicAttribute.hpp"
#include <Common/Core/Core.hpp>

namespace Desert::Geometry
{

    template <typename AttribValueType, int AttribDimension>
    class DynamicMeshTriangleAttribute;

    /**
     * DynamicMeshTriangleAttribute is an add-on to a DynamicMesh3 that allows for
     * per-triangle storage of an attribute value.
     *
     * The DynamicMesh3 mesh topology operations (eg split/flip/collapse edge, poke face, etc)
     * can be mirrored to the overlay via OnSplitEdge(), etc.
     */
    template <typename AttribValueType, int AttribDimension>
    class DynamicMeshTriangleAttribute : public DynamicMeshAttributeBase
    {

    protected:
        /** The parent mesh this overlay belongs to */
        DynamicMesh3* m_ParentMesh = nullptr;

        /** List of per-triangle attribute values */
        DynamicVector<AttribValueType> m_AttribValues;

        using Super = DynamicMeshAttributeBase;

        friend class DynamicMesh3;
        friend class DynamicMeshAttributeSet;

    public:
        /** Create an empty overlay */
        DynamicMeshTriangleAttribute() = default;

        /** Create an overlay for the given parent mesh */
        DynamicMeshTriangleAttribute( DynamicMesh3* ParentMeshIn, bool bAutoInit = true )
             : m_ParentMesh( ParentMeshIn )
        {

            if ( bAutoInit )
            {
                Initialize();
            }
        }

    private:
        /** @set the parent mesh for this overlay.  Only safe for use during FDynamicMesh move */
        void Reparent( DynamicMesh3* ParentMeshIn ) override
        {
            m_ParentMesh = ParentMeshIn;
        }

    public:
        /** @return the parent mesh for this overlay */
        [[nodiscard]] const DynamicMesh3* GetParentMesh() const
        {
            return m_ParentMesh;
        }
        /** @return the parent mesh for this overlay */
        DynamicMesh3* GetParentMesh()
        {
            return m_ParentMesh;
        }

        std::unique_ptr<DynamicMeshAttributeBase> MakeNew( DynamicMesh3* ParentMeshIn ) const override
        {
            auto Matching =
                 std::make_unique<DynamicMeshTriangleAttribute<AttribValueType, AttribDimension>>( ParentMeshIn );
            Matching->Initialize();
            return Matching;
        }

        std::unique_ptr<DynamicMeshAttributeBase> MakeCopy( DynamicMesh3* ParentMeshIn ) const override
        {
            auto ToFill =
                 std::make_unique<DynamicMeshTriangleAttribute<AttribValueType, AttribDimension>>( ParentMeshIn );
            ToFill->Copy( *this );
            return ToFill;
        }

        /** Set this overlay to contain the same arrays as the copy overlay */
        void Copy( const DynamicMeshTriangleAttribute<AttribValueType, AttribDimension>& Copy )
        {
            CopyParentClassData( Copy );
            m_AttribValues = Copy.m_AttribValues;
        }

        std::unique_ptr<DynamicMeshAttributeBase> MakeCompactCopy( const DynamicMeshCompactMaps& CompactMaps,
                                                                   DynamicMesh3* ParentMeshIn ) const override
        {
            auto ToFill =
                 std::make_unique<DynamicMeshTriangleAttribute<AttribValueType, AttribDimension>>( ParentMeshIn );
            ToFill->Initialize();
            ToFill->CompactCopy( CompactMaps, *this );
            return ToFill;
        }

        void CompactInPlace( const DynamicMeshCompactMaps& CompactMaps ) override
        {
            for ( int TID = 0, NumTID = CompactMaps.NumTriangleMappings(); TID < NumTID; TID++ )
            {
                const int ToTID = CompactMaps.GetTriangleMapping( TID );
                if ( ToTID == DynamicMeshCompactMaps::InvalidID )
                {
                    continue;
                }
                if ( Common::EnsureOrWarn( ToTID <= TID, "ToTID <= TID" ) )
                {
                    CopyValue( TID, ToTID );
                }
            }
            m_AttribValues.Resize( m_ParentMesh->MaxTriangleID() * AttribDimension );
        }

        void CompactCopy( const DynamicMeshCompactMaps&                                         CompactMaps,
                          const DynamicMeshTriangleAttribute<AttribValueType, AttribDimension>& ToCopy )
        {
            CopyParentClassData( ToCopy );
            assert( CompactMaps.NumTriangleMappings() <= int( ToCopy.m_AttribValues.Num() / AttribDimension ) );
            AttribValueType Data[AttribDimension];
            for ( int TID = 0, NumTID = CompactMaps.NumTriangleMappings(); TID < NumTID; TID++ )
            {
                const int ToTID = CompactMaps.GetTriangleMapping( TID );
                if ( ToTID == DynamicMeshCompactMaps::InvalidID )
                {
                    continue;
                }
                ToCopy.GetValue( TID, Data );
                SetValue( ToTID, Data );
            }
        }

        /** Initialize the attribute values with InitialValue, and resize to the parent mesh's max triangle ID */
        void Initialize( AttribValueType InitialValue )
        {
            assert( m_ParentMesh != nullptr );
            m_AttribValues.Resize( m_ParentMesh->MaxTriangleID() * AttribDimension );
            m_AttribValues.Fill( InitialValue );
        }

        void Initialize()
        {
            Initialize( GetDefaultAttributeValue() );
        }

        void SetNewValue( int NewTriangleID, const AttribValueType* Data )
        {
            const int k = NewTriangleID * AttribDimension;
            for ( int i = 0; i < AttribDimension; ++i )
            {
                m_AttribValues.InsertAt( Data[i], k + i );
            }
        }

        //
        // Accessors/Queries
        //

        bool Append( const DynamicAttributeBase& Source, const DynamicMesh3::AppendInfo& Info ) override
        {
            int32_t const NewMaxID = Info.NumTriangle + Info.TriangleOffset;
            if ( NewMaxID * AttribDimension > m_AttribValues.Num() )
            {
                m_AttribValues.SetNum( NewMaxID * AttribDimension );
            }

            AttribValueType BufferData[AttribDimension];
            const int       BufferSize = sizeof( BufferData );
            for ( int32_t Idx = 0; Idx < Info.NumTriangle; ++Idx )
            {
                if ( !Common::EnsureOrWarn( Source.CopyOut( Idx, BufferData, BufferSize ),
                                            "Source.CopyOut( Idx, BufferData, BufferSize )" ) )
                {
                    return false;
                }
                SetValue( Idx + Info.TriangleOffset, BufferData );
            }
            return true;
        }

        void AppendDefaulted( const DynamicMesh3::AppendInfo& Info ) override
        {
            int32_t const NewMaxID = Info.NumTriangle + Info.TriangleOffset;
            m_AttribValues.SetMinimumSize( NewMaxID * AttribDimension, GetDefaultAttributeValue() );
        }

        bool CopyOut( int RawID, void* Buffer, int BufferSize ) const override
        {
            if ( sizeof( AttribValueType ) * AttribDimension != BufferSize )
            {
                return false;
            }
            auto*     BufferData = static_cast<AttribValueType*>( Buffer );
            const int k          = RawID * AttribDimension;
            for ( int i = 0; i < AttribDimension; ++i )
            {
                BufferData[i] = m_AttribValues[k + i];
            }
            return true;
        }
        bool CopyIn( int RawID, void* Buffer, int BufferSize ) override
        {
            if ( sizeof( AttribValueType ) * AttribDimension != BufferSize )
            {
                return false;
            }
            auto*     BufferData = static_cast<AttribValueType*>( Buffer );
            const int k          = RawID * AttribDimension;
            for ( int i = 0; i < AttribDimension; ++i )
            {
                m_AttribValues[k + i] = BufferData[i];
            }
            return true;
        }

        /** Get the element at a given index */
        void GetValue( int TriangleID, AttribValueType* Data ) const
        {
            const int k = TriangleID * AttribDimension;
            for ( int i = 0; i < AttribDimension; ++i )
            {
                Data[i] = m_AttribValues[k + i];
            }
        }

        /** Get the element at a given index */
        template <typename AsType>
        void GetValue( int TriangleID, AsType& Data ) const
        {
            int k = TriangleID * AttribDimension;
            for ( int i = 0; i < AttribDimension; ++i )
            {
                Data[i] = m_AttribValues[k + i];
            }
        }

        /** Set the element at a given index */
        void SetValue( int TriangleID, const AttribValueType* Data )
        {
            int k = TriangleID * AttribDimension;
            for ( int i = 0; i < AttribDimension; ++i )
            {
                m_AttribValues[k + i] = Data[i];
            }
        }

        /** Set the element at a given index */
        template <typename AsType>
        void SetValue( int TriangleID, const AsType& Data )
        {
            const int k = TriangleID * AttribDimension;
            for ( int i = 0; i < AttribDimension; ++i )
            {
                m_AttribValues[k + i] = Data[i];
            }
        }

        /** Set the element at a given index with a scalar value (the same value for each dimension) */
        void SetScalarValue( int TriangleID, const AttribValueType& SingleValue )
        {
            const int k = TriangleID * AttribDimension;
            for ( int i = 0; i < AttribDimension; ++i )
            {
                m_AttribValues[k + i] = SingleValue;
            }
        }

        /**
         * Copy the attribute value at FromTriangleID to ToTriangleID
         */
        void CopyValue( int FromTriangleID, int ToTriangleID )
        {
            const int kA = FromTriangleID * AttribDimension;
            const int kB = ToTriangleID * AttribDimension;
            for ( int i = 0; i < AttribDimension; ++i )
            {
                m_AttribValues.InsertAt( m_AttribValues[kA + i], kB + i );
            }
        }

        /** Returns true if the parent-mesh edge is a "Seam" in this overlay */
        [[nodiscard]] bool IsBorderEdge( int EdgeID, bool bMeshBoundaryIsBorder = true ) const
        {
            Index2i EdgeTris = m_ParentMesh->GetEdgeT( EdgeID );
            if ( EdgeTris.B == DynamicMesh3::InvalidID )
            {
                return bMeshBoundaryIsBorder;
            }
            int kA = EdgeTris.A * AttribDimension;
            int kB = EdgeTris.B * AttribDimension;
            for ( int i = 0; i < AttribDimension; ++i )
            {
                if ( m_AttribValues[kA + i] != m_AttribValues[kB + i] )
                {
                    return true;
                }
            }
            return false;
        }

        /** Update the overlay to reflect an edge split in the parent mesh */
        void OnSplitEdge( const DynamicMeshInfo::EdgeSplitInfo& SplitInfo ) override
        {
            CopyValue( SplitInfo.OriginalTriangles.A, SplitInfo.NewTriangles.A );
            if ( SplitInfo.OriginalTriangles.B != DynamicMesh3::InvalidID )
            {
                CopyValue( SplitInfo.OriginalTriangles.B, SplitInfo.NewTriangles.B );
            }
        }

        /** Update the overlay to reflect an edge flip in the parent mesh */
        void OnFlipEdge( const DynamicMeshInfo::EdgeFlipInfo& /*FlipInfo*/ ) override
        {
            // yikes! triangles did not actually change so we will leave attrib unmodified
        }

        /** Update the overlay to reflect an edge collapse in the parent mesh */
        void OnCollapseEdge( const DynamicMeshInfo::EdgeCollapseInfo& /*CollapseInfo*/ ) override
        {
            // nothing to do here, triangles were only deleted
        }

        /** Update the overlay to reflect a face poke in the parent mesh */
        void OnPokeTriangle( const DynamicMeshInfo::PokeTriangleInfo& PokeInfo ) override
        {
            CopyValue( PokeInfo.OriginalTriangle, PokeInfo.NewTriangles.A );
            CopyValue( PokeInfo.OriginalTriangle, PokeInfo.NewTriangles.B );
        }

        /** Update the overlay to reflect an edge merge in the parent mesh */
        void OnMergeEdges( const DynamicMeshInfo::MergeEdgesInfo& /*MergeInfo*/ ) override
        {
            // nothing to do here because triangles did not change
        }

        void OnMergeVertices( const DynamicMeshInfo::MergeVerticesInfo& /*MergeInfo*/ ) override
        {
            // This resolves as either an edge collapse, edge weld, or merge of disconnected vertices.
            //  The triangles either get removed or unchanged- nothing more to do here.
        }

        /** Update the overlay to reflect a vertex split in the parent */
        void OnSplitVertex( const DynamicMeshInfo::VertexSplitInfo& /*SplitInfo*/,
                            const std::span<const int>& /*TrianglesToUpdate*/ ) override
        {
            // nothing to do here because triangles did not change
        }

        virtual AttribValueType GetDefaultAttributeValue()
        {
            return AttribValueType();
        }

        void ResizeAttribStoreIfNeeded( int TriangleID )
        {
            if ( !Common::EnsureOrWarn( TriangleID >= 0, "TriangleID >= 0" ) )
            {
                return;
            }
            const size_t NeededSize = ( ( static_cast<size_t>( TriangleID ) + 1 ) * AttribDimension );
            if ( NeededSize > m_AttribValues.Num() )
            {
                m_AttribValues.Resize( NeededSize, GetDefaultAttributeValue() );
            }
        }

        void OnNewTriangle( int TriangleID, bool /*bInserted*/ ) override
        {
            ResizeAttribStoreIfNeeded( TriangleID );
            SetScalarValue( TriangleID, GetDefaultAttributeValue() );
        }

        /**
         * Returns true if this AttributeSet is the same as Other.
         */
        [[nodiscard]] bool IsSameAs( const DynamicMeshTriangleAttribute<AttribValueType, AttribDimension>& Other,
                                     bool bIgnoreDataLayout ) const
        {
            if ( !bIgnoreDataLayout )
            {
                if ( m_AttribValues.Num() != Other.AttribValues.Num() )
                {
                    return false;
                }

                for ( int Idx = 0, NumValues = m_AttribValues.Num(); Idx < NumValues; Idx++ )
                {
                    if ( m_AttribValues[Idx] != Other.AttribValues[Idx] )
                    {
                        return false;
                    }
                }
            }
            else
            {
                RefCountVector::IndexIterator       ItTid = m_ParentMesh->GetTrianglesRefCounts().BeginIndices();
                const RefCountVector::IndexIterator ItTidEnd = m_ParentMesh->GetTrianglesRefCounts().EndIndices();
                RefCountVector::IndexIterator       ItTidOther =
                     Other.ParentMesh->GetTrianglesRefCounts().BeginIndices();
                const RefCountVector::IndexIterator ItTidEndOther =
                     Other.ParentMesh->GetTrianglesRefCounts().EndIndices();

                while ( ItTid != ItTidEnd && ItTidOther != ItTidEndOther )
                {
                    for ( int32_t i = 0; i < AttribDimension; ++i )
                    {
                        const AttribValueType AttribValue = m_AttribValues[*ItTid * AttribDimension + i];
                        const AttribValueType AttribValueOther =
                             Other.AttribValues[*ItTidOther * AttribDimension + i];
                        if ( AttribValue != AttribValueOther )
                        {
                            // Triangle attribute value is not the same.
                            return false;
                        }
                    }
                    ++ItTid;
                    ++ItTidOther;
                }

                if ( ItTid != ItTidEnd || ItTidOther != ItTidEndOther )
                {
                    // Number of triangle attribute values is not the same.
                    return false;
                }
            }

            return true;
        }

        /**
         * Check validity of attribute
         *
         * @param bAllowNonmanifold Accept non-manifold topology as valid. Note that this should almost always be
         * true for attributes; non-manifold overlays are generally valid.
         * @param FailMode Desired behavior if mesh is found invalid
         */
        [[nodiscard]] bool CheckValidity( bool /*bAllowNonmanifold*/,
                                          ValidityCheckFailMode FailMode ) const override
        {
            // just check that the values buffer is big enough
            if ( ( m_ParentMesh == nullptr ) || m_ParentMesh->MaxTriangleID() < 0 ||
                 static_cast<size_t>( m_ParentMesh->MaxTriangleID() ) * AttribDimension > m_AttribValues.Num() )
            {
                switch ( FailMode )
                {
                    case ValidityCheckFailMode::Check:
                        assert( false );
                        return false;
                    case ValidityCheckFailMode::Ensure:
                        DESERT_VERIFY_WARN( false );
                        return false;
                    default:
                        return false;
                }
            }

            return true;
        }

        [[nodiscard]] size_t GetByteCount() const override
        {
            return m_AttribValues.GetByteCount();
        }
    };

    /**
     * DynamicMeshScalarTriangleAttribute is an extension of DynamicMeshTriangleAttribute for scalar-valued
     * attributes. Adds some convenience functions to simplify get/set code.
     */
    template <typename RealType>
    class DynamicMeshScalarTriangleAttribute : public DynamicMeshTriangleAttribute<RealType, 1>
    {
    public:
        using BaseType = DynamicMeshTriangleAttribute<RealType, 1>;
        using BaseType::GetValue;
        using BaseType::SetNewValue;
        using BaseType::SetValue;

        DynamicMeshScalarTriangleAttribute() : BaseType()
        {
        }

        DynamicMeshScalarTriangleAttribute( DynamicMesh3* ParentMeshIn ) : BaseType( ParentMeshIn )
        {
        }

        void SetNewValue( int NewTriangleID, RealType Value )
        {
            this->AttribValues.InsertAt( Value, NewTriangleID );
        }

        [[nodiscard]] RealType GetValue( int TriangleID ) const
        {
            return this->m_AttribValues[TriangleID];
        }

        void SetValue( int TriangleID, RealType Value )
        {
            this->m_AttribValues[TriangleID] = Value;
        }
    };

    template <typename AttribValueType>
    class DynamicMeshSingleTriangleAttribute : public DynamicMeshTriangleAttribute<AttribValueType, 1>
    {
        using BaseType = DynamicMeshTriangleAttribute<AttribValueType, 1>;

    public:
        DynamicMeshSingleTriangleAttribute() = default;

        DynamicMeshSingleTriangleAttribute( DynamicMesh3* ParentMeshIn, bool bAutoInit = true )
             : BaseType( ParentMeshIn, bAutoInit )
        {
        }

        AttribValueType GetValue( int TriangleID ) const
        {
            return this->AttribValues[TriangleID];
        }

        void SetValue( int TriangleID, const AttribValueType& Value )
        {
            this->AttribValues[TriangleID] = Value;
        }
    };

} // namespace Desert::Geometry
