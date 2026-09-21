#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace Desert::Assets
{
    // OWNED BY A shared_ptr, AND IT CAN NOW SAY SO — `weak_from_this()`.
    //
    // Not decoration. `MeshService::RegisterAsset` requires a `weak_ptr<AssetManager>` because a lazily
    // registered `.stmesh` shell defers its parse, and the parse is the first moment a `.skmesh` learns
    // which skeleton it needs; without a registry to ask, that deferred load fails and the mesh is
    // invisible. Until this line, the ONLY caller able to satisfy that requirement was `AssetPreloader`,
    // which happens to hold the `weak_ptr` the layer gave it. Everybody else — including the scene parse,
    // which is where a scene's own meshes are resolved — had a bare `const AssetManager&` and could
    // therefore only use the EAGER `Register`, whose deferred-load path then failed for exactly the same
    // reason with `no AssetManager is bound`. MEASURED: with the preloader's registration loop switched
    // off, `M10_MeshSlot` renders no mesh and prints that error once per frame, ninety times.
    //
    // So the requirement was satisfiable by one caller by accident of who held what, which is the same
    // "it works because somebody else did the work earlier" this file's neighbours keep paying for. A
    // registry that can hand out its own weak reference makes it satisfiable by construction.
    //
    // STILL HEADER-ONLY (a base class needs no translation unit), which eleven suites depend on. A
    // manager built on the stack — as those suites build it — returns an EMPTY weak_ptr here, and
    // `RegisterAsset` refuses that by name rather than storing a reference that can never lock.
    class AssetManager final : public std::enable_shared_from_this<AssetManager>
    {
    public:
        /**
         * @brief EVERY REGISTRY THAT IS CURRENTLY ALIVE, in creation order.
         *
         * The same arrangement, for the same reason, as `Core::Scene::LiveScenes()`: asset eviction runs
         * from the engine's frame loop, which has no way to be handed a registry — the thing that OWNS one
         * is the editor or runtime layer, and both are above this layer. A registry that puts itself in a
         * list is the only shape in which "sweep the registries" is answerable from below.
         *
         * Raw pointers, non-owning, entered by the constructor and removed by the destructor, so an entry
         * can never outlive its object. There is one registry per project today; a project switch that
         * built a second before releasing the first would simply have both swept, which is correct.
         */
        //
        // HEADER-ONLY, AND THAT IS LOAD-BEARING RATHER THAN A STYLE CHOICE. This class had no translation
        // unit of its own, and eleven test suites rely on that: they compile a hand-picked list of asset
        // sources precisely so that a registry can be constructed without linking the renderer. Giving it
        // a `.cpp` broke every one of them with an undefined `AssetManager::AssetManager()` — caught by
        // the sweep, one commit after it was written. The list below stays inline for the same reason.
        [[nodiscard]] static std::vector<AssetManager*>& LiveManagerList()
        {
            static std::vector<AssetManager*> managers;
            return managers;
        }

        [[nodiscard]] static const std::vector<AssetManager*>& LiveManagers()
        {
            return LiveManagerList();
        }

        AssetManager()
        {
            LiveManagerList().push_back( this );
        }

        ~AssetManager()
        {
            auto& managers = LiveManagerList();
            managers.erase( std::remove( managers.begin(), managers.end(), this ), managers.end() );
        }

        // Deleted for the reason the list makes newly load-bearing: a copy would enter a second pointer to
        // one logical registry, a move would leave the moved-from husk in the list.
        AssetManager( const AssetManager& )            = delete;
        AssetManager& operator=( const AssetManager& ) = delete;
        AssetManager( AssetManager&& )                 = delete;
        AssetManager& operator=( AssetManager&& )      = delete;

        // `using KeyHandle = Common::Filepath;` stood here with ZERO users anywhere in the repository. It
        // was the old spelling of "what this registry is keyed on", and leaving a name that says a path
        // is the key next to a class that has just stopped keying on paths is how the next reader gets
        // the wrong answer for free. `AssetKey` is that name now.
        using AssetContainer = std::vector<std::pair<AssetMetadata, Asset<AssetBase>>>;
        using AssetIndex     = uint32_t;

        template <typename AssetType, typename... Args>
        Asset<AssetType> CreateAsset( const AssetPriority priority, const Common::Filepath& filepath,
                                      bool loadAfterCreate = true, Args&&... args )
        {
            static_assert( std::is_base_of_v<AssetBase, AssetType>, "AssetType must inherit from AssetBase" );

            // ONE lookup, on the asset's identity key, and the key is built once per registration.
            //
            // This used to be a linear scan comparing raw paths. Deduplicating on the identity key is
            // what stops a second SPELLING of an already-registered file becoming a second record that
            // then takes over the handle — but computing that key inside a scan is quadratic in the
            // number of assets AND in the work per comparison: measured over a preload of 2000 assets,
            // 56.9 s against 207 ms for the raw-path scan. Asking the question once and hashing it makes
            // the same preload 30 ms, i.e. faster than the version this replaces, because a string
            // compare beats a std::filesystem::path compare.
            const AssetKey key( filepath, AssetType::GetTypeID() );

            if ( const Asset<AssetBase>* record = FindRecord( key ) )
            {
                return AsRequestedType<AssetType>( *record, "CreateAsset", key.Value() );
            }

            // NOTE:Perhaps the creation of an asset via the Create() method should be defined for each type
            // separately, and then call AssetType::Create()
            auto asset = std::make_shared<AssetType>( priority, filepath, std::forward<Args>( args )... );
            if ( loadAfterCreate )
            {
                const auto& loadResult = asset->Load();
                if ( !loadResult )
                {
                    LOG_ERROR( "Error while loading {}. Error: {}", filepath.string(), loadResult.GetError() );
                    return nullptr;
                }
            }

            const auto& metadata = asset->GetMetadata();
            m_AssetsCache.push_back( { metadata, asset } );
            m_HandleLookup[metadata.Handle] = m_AssetsCache.size() - 1;
            // Keyed on the LOOKUP's key, not on the stored metadata's: Texture2D and Material replace
            // their handle from an id inside the file during Load above, but their PATH is unchanged, so
            // both spellings resolve here either way. Using the record's own key would be the same
            // string; using the lookup's says plainly which question this map answers.
            m_PathLookup.emplace( key, m_AssetsCache.size() - 1 );

            // AND THE COOKED REGISTRY LEARNS ABOUT THE FILE HERE, at the moment it becomes an asset.
            //
            // The same argument `AssetHandle::FromCookedPath` makes for recording its own inverse where
            // the number is derived: this is the one place a content file becomes a thing the engine
            // has, so a row written here is TOTAL by construction and there is no middle link to drop
            // it. Every other way of building the list — a walk somebody must remember to run, a
            // per-type table somebody must remember to extend — is a SECOND list, and this repository
            // has paid for that twice (the packager's hand-typed tree list that forgot fonts and icons,
            // and the twelve-branch ToPath that returns "" for a type nobody added a branch for).
            //
            // AFTER `Load()` above, which is what makes the DECLARED identity reach the row: a `.tex`
            // and a `.demat` replace their path-derived handle with the id inside the file, and the row
            // has to carry the number a scene reference actually holds. A path that names no content
            // kind (`procedural://`, a `.dgraph` document) is ignored by NoteAsset, not refused.
            ContentRegistry::NoteAsset( metadata.Filepath, static_cast<uint64_t>( metadata.Handle ) );

            if constexpr ( std::is_base_of_v<AssetBase, AssetType> )
            {
                asset->ResolveDependencies( *this );
            }

            return asset;
        }

        // WHAT A MISS MEANS — and only the caller knows. See ProbeByHandle below for the whole story.
        enum class LookupReport
        {
            Named,   ///< a mismatch is a symptom: somebody holds the wrong handle. Reported.
            Probing, ///< a mismatch is the expected answer to "is it one of these?". Trace only.
        };

        template <typename TypeAsset>
        Asset<TypeAsset> FindByHandle( const AssetHandle& handle ) const
        {
            if ( auto it = m_HandleLookup.find( handle ); it != m_HandleLookup.end() )
            {
                return AsRequestedType<TypeAsset>( m_AssetsCache[it->second].second, "FindByHandle",
                                                   std::to_string( static_cast<uint64_t>( handle ) ) );
            }
            return nullptr;
        }

        // "IS THIS HANDLE A TypeAsset? I EXPECT MOST ANSWERS TO BE NO."
        //
        // Same lookup as FindByHandle and the same refusal to reinterpret — the ONLY difference is that
        // the caller has declared, in the name it wrote, that a mismatch is the expected answer rather
        // than a symptom. A walk over mixed handles asking each of them several typed questions in turn
        // (Engine/Assets/AssetEviction.cpp does exactly this, once per reachable handle per round)
        // produces one refusal per question that does not apply, and every one of those used to be an
        // ERROR line about work going exactly right: four per scene load in this tree.
        //
        // The severity could not be derived down in AsRequestedType, and that is the point worth keeping.
        // Its discriminator — does the registry's recorded type id agree with the one asked for — cannot
        // tell a probe from a confusion, because in BOTH the ids disagree. The difference is not in the
        // data at all; it is in what the caller meant, so the caller is the only place it can be said.
        // Use FindByHandle when a miss means something is wrong, and this when a miss means "not that
        // one, next question".
        template <typename TypeAsset>
        Asset<TypeAsset> ProbeByHandle( const AssetHandle& handle ) const
        {
            if ( auto it = m_HandleLookup.find( handle ); it != m_HandleLookup.end() )
            {
                return AsRequestedType<TypeAsset>( m_AssetsCache[it->second].second, "ProbeByHandle",
                                                   std::to_string( static_cast<uint64_t>( handle ) ),
                                                   LookupReport::Probing );
            }
            return nullptr;
        }

        // "WHICH FILE IS THIS HANDLE?" — asked without claiming to know what type the handle names.
        //
        // Every lookup above is TYPED, and rightly so: a typed question that cannot be answered must be
        // refused rather than reinterpreted, which is what AsRequestedType is for. But that leaves one
        // question with no answer at all — a caller holding only a handle, for instance the editor
        // reporting that a document could NOT be opened, has no type to ask with and needs a name a person
        // recognises rather than a decimal id.
        //
        // Returning METADATA and not an asset is what makes this safe to answer untyped: there is nothing
        // here to cast, so the failure mode the typed lookups exist to prevent — a plausible pointer to an
        // object of another class — cannot occur. The metadata's own AssetType says what the record is,
        // and the caller may not assume anything else about it. Null for a handle that is not registered.
        [[nodiscard]] const AssetMetadata* FindMetadataByHandle( const AssetHandle& handle ) const
        {
            if ( auto it = m_HandleLookup.find( handle ); it != m_HandleLookup.end() )
                return &m_AssetsCache[it->second].first;
            return nullptr;
        }

        // "DOES THE REGISTRY ALREADY HOLD THIS FILE, AS THIS TYPE?" — the SAME question CreateAsset asks
        // before it decides to build one, answered by the same key through the same map.
        //
        // That sentence is the whole of this change. It used to be a linear scan comparing
        // `AssetMetadata::Filepath` VERBATIM, so the registry gave two answers to one question and the
        // 44 call sites of this function were reading the weaker one. The census taken before the fix
        // (commit "Ф5, перепись до правки") found that not one of those 44 wanted spelling sensitivity:
        // every one of them means "is there such an asset", 36 recover from a wrong "no" by calling
        // CreateAsset — which is how the disagreement stayed survivable for so long — and 8 have no
        // recovery at all and silently drop the reference or log an error about a file that is loaded.
        //
        // What a wrong "no" cost when it was finally paid: a scene naming `Cooked/Meshes/base.stmesh`
        // missed here, called CreateAsset, was handed AssetPreloader's UNPARSED shell for the absolute
        // spelling of that same file, and registered it as if it were fresh — an empty mesh cached under
        // a live handle, `Get` answering zero submeshes 91 times in a 90-frame run.
        //
        // Also strictly faster, which is worth saying because the shape looks like it should be slower:
        // this was O(assets) `std::filesystem::path` comparisons per question and is now one
        // StableKeyForPath plus one hash. On the numbers already measured for this registry (2 M path
        // compares in 207 ms, i.e. ~0.1 us each) a single question over a 2000-asset project cost ~200 us
        // and now costs ~15-30 us — and MeshDnD asks it once per file of a recursive directory scan.
        template <typename TypeAsset>
        Asset<TypeAsset> FindByPath( const Common::Filepath& path ) const
        {
            const AssetKey key( path, TypeAsset::GetTypeID() );

            if ( const Asset<AssetBase>* record = FindRecord( key ) )
            {
                return AsRequestedType<TypeAsset>( *record, "FindByPath", key.Value() );
            }
            return nullptr;
        }

        template <typename TypeAsset>
        std::vector<std::pair<AssetHandle, Asset<TypeAsset>>> FindAllByType() const
        {
            std::vector<std::pair<AssetHandle, Asset<TypeAsset>>> result;
            const auto                                            typeId = TypeAsset::GetTypeID();

            for ( const auto& [metadata, asset] : m_AssetsCache )
            {
                if ( metadata.AssetType == typeId )
                {
                    // Filtered on the REGISTRY's copy of the type, then checked against the ASSET's own:
                    // the two are written at different moments and a census that trusted only the copy
                    // would count a record the copy mislabels. AsRequestedType refuses and says so.
                    if ( auto casted = AsRequestedType<TypeAsset>( asset, "FindAllByType",
                                                                   metadata.Filepath.generic_string() ) )
                    {
                        result.emplace_back( metadata.Handle, casted );
                    }
                }
            }

            return result;
        }

    private:
        // A TYPED LOOKUP ANSWERS OR REFUSES. It never reinterprets.
        //
        // Every Find* above used to end in `sp_cast`, i.e. `std::static_pointer_cast`, and that is not a
        // question — it is an ASSERTION that the record holds the requested class. When the assertion was
        // wrong the caller got a perfectly non-null pointer to an object of another class, so every `if
        // (!asset)` downstream waved it through and the code read a stranger's memory as its own. It was
        // observed: a `SkyboxAsset` came back from a `CloudTypeAsset` request, non-null, in the
        // AssetHandleStability suite. A wrong answer that is indistinguishable from a right one is the
        // worst thing a lookup can return, and no caller can defend against it.
        //
        // Today's key carries the type, so nothing collides today. This exists because the FAILURE MODE,
        // not the collision, is the defect: any future collision — a hash meeting, an id read out of a
        // file, an importer key too coarse to separate two assets — would again produce a plausible
        // pointer instead of an error.
        //
        // WHY `dynamic_pointer_cast` AND NOT THE STORED `AssetTypeID`. Comparing `TypeAsset::GetTypeID()`
        // against the record's own `AssetTypeID` is one integer compare and needs no RTTI, and it is not
        // enough: `AssetTypeID::Mesh` covers TWO C++ classes. `StaticMeshAsset` and `SkinnedMeshAsset` both
        // report `Mesh` — neither declares its own `GetTypeID()` — so an id compare passes a skinned record
        // to `FindByHandle<StaticMeshAsset>` and `static_pointer_cast` reinterprets it. That is reachable
        // from the editor, not hypothetical: the static mesh picker lists `FindAllByType<MeshAsset>()`,
        // which is every mesh including the skinned ones, and `StaticMeshComponent` then resolves the
        // chosen handle as `StaticMeshAsset`. An id compare would close the collision this task was given
        // and leave that one open.
        //
        // MEASURED, not estimated, because this sits on the preload path where the neighbouring lookup
        // already cost 56.9 s on 2000 assets when it was written the naive way. Over 1e6 lookups on the
        // real asset classes, Release, arm64: `static_pointer_cast` with no check 10.5 ns, id compare
        // 5.6 ns (cheaper than the baseline — the refusing half never constructs a shared_ptr),
        // `dynamic_pointer_cast` 15.0 ns. RTTI therefore costs ~4.5 ns per lookup, i.e. ~9 microseconds
        // over a 2000-asset preload, against a defect class that has already cost this programme days.
        // The id is kept for the MESSAGE, where naming the two asset types is what a human can act on.
        //
        // The mismatch is NAMED, not merely turned into null: `who` is the entry point and `subject` the
        // key that was asked about, so the log line says which question produced a stranger and which
        // record the stranger was. `typeid` is there for the case the two asset type NAMES are equal —
        // exactly the static-versus-skinned mesh above, where "Mesh was requested as Mesh" would tell the
        // reader nothing.
        //
        // AND THE SEVERITY IS DERIVED, not fixed: the same equal-names case is ALSO the case where the
        // miss is expected rather than wrong, so it is reported at trace. See the branch below — the
        // discriminator is whether the registry's recorded type id agrees with the requested one.
        template <typename TypeAsset>
        static Asset<TypeAsset> AsRequestedType( const Asset<AssetBase>& stored, const char* who,
                                                 const std::string& subject,
                                                 LookupReport       report = LookupReport::Named )
        {
            static_assert( std::is_base_of_v<AssetBase, TypeAsset>, "TypeAsset must inherit from AssetBase" );

            if ( !stored )
            {
                return nullptr;
            }

            auto typed = std::dynamic_pointer_cast<TypeAsset>( stored );
            if ( !typed )
            {
                // The dereference is bound to a reference first because `typeid` on an expression WITH
                // SIDE EFFECTS evaluates it, and `*stored` is `shared_ptr::operator*` — a function call,
                // so the operand is not the plain lvalue it reads as. Same dynamic type, intent stated.
                const AssetBase& storedRef = *stored;

                // TWO FAILURES WEAR ONE FACE HERE, AND ONLY ONE OF THEM IS A DEFECT.
                //
                // If the registry's recorded type id DISAGREES with the type asked for, the caller went
                // looking for a Material and found a Mesh: somebody is holding the wrong handle, or a
                // record is mislabelled. That is worth an ERROR and always was.
                //
                // If the two type ids AGREE and the cast still failed, nothing is wrong at all. Several
                // asset CLASSES share one type id — StaticMeshAsset and SkinnedMeshAsset are both
                // `Mesh` — so `FindAllByType<SkinnedMeshAsset>()` walks past every static mesh in the
                // project by design, and each miss used to produce a line reading "holds a Mesh asset but
                // was requested as Mesh". Two per scene load in this tree, at ERROR, saying what looks
                // like a typo. Real ERROR lines are only worth reading if they are all real; a routine
                // subtype probe is not one, so it goes to trace and NAMES BOTH CLASSES rather than the
                // shared type name that made the pair indistinguishable.
                // AND THERE IS A THIRD CASE THAT ONLY THE CALLER CAN DECLARE. The two branches below
                // guess at intent from the type ids, which is all this function can see. A caller that
                // asks "is this handle a Mesh? a Material? a CloudType?" about EVERY handle it holds —
                // the eviction root walk does exactly that — expects a mismatch on nearly every question
                // and a DISAGREEING type id on most of them, so the ERROR branch fires four times per
                // scene load about work that is going exactly right. No discriminator computed here can
                // separate that from a real confusion, because the difference is not in the data: it is
                // in what the caller meant. So the caller says so, with ProbeByHandle, and this stays
                // quiet for it. See LookupReport above the entry points.
                if ( report == LookupReport::Probing )
                {
                    LOG_TRACE( "AssetManager::{}: '{}' is a '{}', not the '{}' being probed for. The caller "
                               "declared this a probe over mixed handles, so a miss is the expected answer.",
                               who, subject, typeid( storedRef ).name(), typeid( TypeAsset ).name() );
                    return nullptr;
                }

                const bool typeIdAgrees = stored->GetMetadata().AssetType == TypeAsset::GetTypeID();
                if ( typeIdAgrees )
                {
                    LOG_TRACE( "AssetManager::{}: '{}' is a '{}', not the '{}' this lookup asked for — both "
                               "are {} assets, so this is a subtype probe passing over it, not an error.",
                               who, subject, typeid( storedRef ).name(), typeid( TypeAsset ).name(),
                               AssetTypeName( TypeAsset::GetTypeID() ) );
                }
                else
                {
                    LOG_ERROR( "AssetManager::{}: '{}' holds a {} asset (type id {}, class '{}') but was "
                               "requested as {} (class '{}'). Refusing to reinterpret it; returning null.",
                               who, subject, AssetTypeName( stored->GetMetadata().AssetType ),
                               static_cast<int>( stored->GetMetadata().AssetType ), typeid( storedRef ).name(),
                               AssetTypeName( TypeAsset::GetTypeID() ), typeid( TypeAsset ).name() );
                }
            }

            return typed;
        }

    public:
        /**
         * @brief Every registered asset's METADATA, in registration order. Read-only, and metadata only.
         *
         * The typed lookups above answer "give me THIS asset, as THIS type", which is right for using one
         * and useless for listing them: a caller that wants to offer the user (or a control-channel
         * client) every material in the project has no handle to ask with and no business loading each
         * one to find out what it is.
         *
         * METADATA AND NOT ASSETS, for the reason FindMetadataByHandle gives next to it: there is nothing
         * here to cast, so the failure the typed lookups exist to prevent — a plausible pointer to an
         * object of another class — cannot occur. The record's own AssetType says what it is.
         *
         * The reference is into the manager's storage and is invalidated by anything that registers a new
         * asset. Callers walk it and copy what they keep; nothing here hands out a handle to hold.
         */
        [[nodiscard]] const AssetContainer& RegisteredAssets() const noexcept
        {
            return m_AssetsCache;
        }

    private:
        // THE ONLY ROUTE FROM A FILE TO A RECORD, and that is the invariant rather than a tidiness.
        //
        // `CreateAsset` and `FindByPath` are the two entry points that ask "which record is this file?",
        // and for as long as each computed its own answer they were free to disagree — one on the
        // identity key, one on a verbatim path compare. They now share this, so a disagreement is not
        // something a test has to catch: there is one comparison and both callers make it.
        //
        // It also closes the door behind itself. `m_AssetsCache` is private and the one public window on
        // it (`RegisteredAssets()`) hands out metadata for listing, not for lookup; `m_PathLookup` is
        // keyed on `AssetKey`, which has no constructor from a string and no implicit conversion from a
        // path, so a third lookup written next year cannot quietly ask the older question — it either
        // calls this or it does not compile.
        [[nodiscard]] const Asset<AssetBase>* FindRecord( const AssetKey& key ) const
        {
            const auto it = m_PathLookup.find( key );
            return it == m_PathLookup.end() ? nullptr : &m_AssetsCache[it->second].second;
        }

        AssetContainer                              m_AssetsCache;
        std::unordered_map<AssetHandle, AssetIndex> m_HandleLookup;
        std::unordered_map<AssetKey, AssetIndex>    m_PathLookup;
    };

    template <typename T>
    struct AssetDependency
    {
        AssetHandle      Handle;
        std::weak_ptr<T> Cached;

        void Resolve( AssetManager& manager )
        {
            auto asset = manager.FindByHandle<T>( Handle );
            Cached     = asset;
        }

        bool IsValid() const
        {
            return !Cached.expired();
        }

        T* Get() const
        {
            if (auto ptr = Cached.lock())
            {
                return ptr.get();
            }

            return nullptr;
        }
    };
} // namespace Desert::Assets