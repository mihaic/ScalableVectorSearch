/**
 *    Copyright (C) 2023-present, Intel Corporation
 *
 *    You can redistribute and/or modify this software under the terms of the
 *    GNU Affero General Public License version 3.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    version 3 along with this software. If not, see
 *    <https://www.gnu.org/licenses/agpl-3.0.en.html>.
 */

#pragma once

// Flat index utilities
#include "svs/index/flat/inserters.h"

// svs
#include "svs/concepts/distance.h"
#include "svs/core/data.h"
#include "svs/core/distance.h"
#include "svs/core/query_result.h"
#include "svs/lib/neighbor.h"
#include "svs/lib/threads.h"
#include "svs/lib/traits.h"
#include "svs/quantization/lvq/lvq.h"

// stdlib
#include <tuple>

namespace svs::index::flat {

// The flat index is "special" because we wish to enable the `FlatIndex` to either:
// (1) Own the data and thread pool.
// (2) Reference an existing dataset and thread pool.
//
// The latter option allows other index implementations like the VamanaIndex to launch a
// scoped `FlatIndex` to perform exhaustive searches on demand (useful when validating
// the behavior of the dynamic index).
//
// To that end, we allow the actual storage of the data and the threadpool to either be
// owning (by value) or non-owning (by reference).
struct OwnsMembers {
    template <typename T> using storage_type = T;
};
struct ReferencesMembers {
    template <typename T> using storage_type = T&;
};

template <typename Ownership, typename T>
using storage_type_t = typename Ownership::template storage_type<T>;

///
/// @brief Implementation of the Flat index.
///
/// @tparam Data The full type of the dataset being indexed.
/// @tparam Dist The distance functor used to compare queries with the elements of the
///     dataset.
/// @tparam Ownership Implementation detail and may be ommitted for most use cases.
///
/// The mid-level implementation for the flat index that uses exhaustive search to find
/// the exact nearest neighbors (within the limitations of possibly quantization error
/// for the dataset or floating-point error for some distance functors).
///
/// **NOTE**: This method is not as performant as other index methods. It is meant to
/// return the exact rather than approximate nearest neighbors and thus must exhaustively
/// search the whole dataset.
///
template <
    data::ImmutableMemoryDataset Data,
    typename Dist,
    typename Ownership = OwnsMembers>
class FlatIndex {
  public:
    using const_value_type = data::const_value_type_t<Data>;

    /// The type of the distance functor.
    using distance_type = Dist;
    /// The type of dataset.
    using data_type = Data;
    using thread_pool_type = threads::NativeThreadPool;
    using compare = distance::compare_t<Dist>;
    using sorter_type = BulkInserter<Neighbor<size_t>, compare>;

    static const size_t default_data_batch_size = 100'000;

    // Compute data and threadpool storage types.
    using data_storage_type = storage_type_t<Ownership, Data>;
    using thread_storage_type = storage_type_t<Ownership, thread_pool_type>;

  private:
    data_storage_type data_;
    [[no_unique_address]] distance_type distance_;
    thread_storage_type threadpool_;

    // Constructs controlling the iteration strategy over the data and queries.
    size_t data_batch_size_ = 0;
    size_t query_batch_size_ = 0;

    // Helpers methods to obtain automatic batch sizing.

    // Automatic behavior: Use the default batch size.
    size_t compute_data_batch_size() const {
        if (data_batch_size_ == 0) {
            return default_data_batch_size;
        } else {
            return std::min(data_batch_size_, data_.size());
        }
    }

    // Automatic behavior: Evenly divide queries over the threads.
    size_t compute_query_batch_size(size_t num_queries) const {
        if (query_batch_size_ == 0) {
            return lib::div_round_up(num_queries, threadpool_.size());
        } else {
            return query_batch_size_;
        }
    }

  public:
    ///
    /// @brief Construct a new index from constituent parts.
    ///
    /// @tparam ThreadPoolProto The type of the threadpool proto type. See notes on the
    ///     corresponding parameter below.
    ///
    /// @param data The data to use for the index. The resulting index will take ownership
    ///     of the passed argument.
    /// @param distance The distance functor to use to compare queries with dataset
    ///     elements.
    /// @param threadpool_proto Something that can be used to build a threadpool using
    ///     ``threads::as_threadpool``. In practice, this means that ``threapool_proto``
    ///     can be either a threadpool directly, or an integer. In the latter case, a new
    ///     threadpool will be constructed using ``threadpool_proto`` as the number of
    ///     threads to create.
    ///
    template <typename ThreadPoolProto>
    FlatIndex(Data data, Dist distance, ThreadPoolProto&& threadpool_proto)
        requires std::is_same_v<Ownership, OwnsMembers>
        : data_{std::move(data)}
        , distance_{std::move(distance)}
        , threadpool_{
              threads::as_threadpool(std::forward<ThreadPoolProto>(threadpool_proto))} {}

    FlatIndex(Data& data, Dist distance, threads::NativeThreadPool& threadpool)
        requires std::is_same_v<Ownership, ReferencesMembers>
        : data_{data}
        , distance_{std::move(distance)}
        , threadpool_{threadpool} {}

    ////// Dataset Interface

    /// Return the number of independent entries in the index.
    size_t size() const { return data_.size(); }

    /// Return the logical number of dimensions of the indexed vectors.
    size_t dimensions() const { return data_.dimensions(); }

    ///
    /// @brief Return the ``num_neighbors`` nearest neighbors to each query.
    ///
    /// @tparam Queries The full type of the queries.
    /// @tparam Pred The type of the optional predicate.
    ///
    /// @param queries The queries. Each entry will be processed.
    /// @param num_neighbors The number of approximate nearest neighbors to return for
    ///     each query.
    /// @param predicate A predicate functor that can be used to exclude certain dataset
    ///     elements from consideration. See
    ///     \ref flat_class_search_mutating "the mutating method" for details.
    ///
    /// @returns A QueryResult containing ``queries.size()`` entries with position-wise
    ///     correspondence to the queries with respect to ``Dist`` and ``predicate``.
    ///
    ///     Row `i` in the result corresponds to the neighbors for the `i`th query.
    ///     Neighbors within each row are ordered from nearest to furthest.
    ///
    /// Internally, this method calls the mutating version of search. See the documentation
    /// of that method for more details.
    ///
    template <
        data::ImmutableMemoryDataset Queries,
        typename Pred = lib::Returns<lib::Const<true>>>
    QueryResult<size_t> search(
        const Queries& queries,
        size_t num_neighbors,
        Pred predicate = lib::Returns(lib::Const<true>())
    ) {
        QueryResult<size_t> result{queries.size(), num_neighbors};
        search(queries.cview(), num_neighbors, result.view(), predicate);
        return result;
    }

    ///
    /// @anchor flat_class_search_mutating
    /// @brief Fill the result with the ``num_neighbors`` nearest neighbors for each query.
    ///
    /// @tparam Queries The full type of the queries.
    /// @tparam Pred The type of the optional predicate.
    ///
    /// @param queries The queries. Each entry will be processed.
    /// @param num_neighbors The number of approximate nearest neighbors to return for
    ///     each query.
    /// @param result The result data structure to populate.
    ///     Row `i` in the result corresponds to the neighbors for the `i`th query.
    ///     Neighbors within each row are ordered from nearest to furthest.
    /// @param predicate A predicate functor that can be used to exclude certain dataset
    ///     elements from consideration. This functor must implement
    ///     ``bool operator()(size_t)`` where the ``size_t`` argument is an index in
    ///     ``[0, data.size())``. If the predicate returns ``true``, that dataset element
    ///     will be considered.
    ///
    /// **Preconditions:**
    ///
    /// The following pre-conditions must hold. Otherwise, the behavior is undefined.
    /// - ``result.n_queries() == queries.size()``
    /// - ``result.n_neighbors() == num_neighbors``.
    /// - The value type of ``queries`` is compatible with the value type of the index
    ///     dataset with respect to the stored distance functor.
    ///
    /// **Implementation Details**
    ///
    /// The internal call stack looks something like this.
    ///
    /// @code{}
    /// search: Prepare scratch space and perform tiling over the dataset.
    ///   |
    ///   +-> search_subset: multi-threaded search of all queries over the current subset
    ///       of the dataset. Partitions up the queries according to query batch size
    ///       and dynamically load balances query partition among worker threads.
    ///         |
    ///         +-> search_patch: Bottom level routine meant to run on a single thread.
    ///             Compute the distances between a subset of the queries and a subset
    ///             of the data and maintines the `num_neighbors` best results seen so far.
    /// @endcode{}
    ///
    template <typename QueryType, typename Pred = lib::Returns<lib::Const<true>>>
    void search(
        const data::ConstSimpleDataView<QueryType>& queries,
        size_t num_neighbors,
        QueryResultView<size_t> result,
        Pred predicate = lib::Returns(lib::Const<true>())
    ) {
        const size_t data_max_size = data_.size();

        // Partition the data into `data_batch_size_` chunks.
        // This will keep all threads at least working on the same sub-region of the dataset
        // to provide somewhat better locality.
        auto data_batch_size = compute_data_batch_size();

        // Allocate query processing space.
        sorter_type scratch{queries.size(), num_neighbors, compare()};
        scratch.prepare();

        size_t start = 0;
        while (start < data_.size()) {
            size_t stop = std::min(data_max_size, start + data_batch_size);
            search_subset(queries, threads::UnitRange(start, stop), scratch, predicate);
            start = stop;
        }

        // By this point, all queries have been compared with all dataset elements.
        // Perform any necessary post-processing on the sorting network and write back
        // the results.
        scratch.cleanup();
        threads::run(
            threadpool_,
            threads::StaticPartition(queries.size()),
            [&](const auto& query_indices, uint64_t /*tid*/) {
                for (auto i : query_indices) {
                    const auto& neighbors = scratch.result(i);
                    for (size_t j = 0; j < num_neighbors; ++j) {
                        const auto& neighbor = neighbors[j];
                        result.index(i, j) = neighbor.id();
                        result.distance(i, j) = neighbor.distance();
                    }
                }
            }
        );
    }

    template <typename QueryType, typename Pred = lib::Returns<lib::Const<true>>>
    void search_subset(
        const data::ConstSimpleDataView<QueryType>& queries,
        const threads::UnitRange<size_t>& data_indices,
        sorter_type& scratch,
        Pred predicate = lib::Returns(lib::Const<true>())
    ) {
        // Process all queries.
        threads::run(
            threadpool_,
            threads::DynamicPartition{
                queries.size(), compute_query_batch_size(queries.size())},
            [&](const auto& query_indices, uint64_t /*tid*/) {
                // Broadcast the distance functor so each thread can process all queries
                // in its current batch.
                distance::BroadcastDistance distances{
                    data_.adapt_distance(distance_), query_indices.size()};

                search_patch(
                    queries,
                    data_indices,
                    threads::UnitRange(query_indices),
                    scratch,
                    distances,
                    predicate
                );
            }
        );
    }

    // Perform all distance computations between the queries and the stored dataset over
    // the cartesian product of `query_indices` x `data_indices`.
    //
    // Insert the computed distance for each query/distance pair into `scratch`, which
    // will maintain the correct number of nearest neighbors.
    template <
        typename QueryType,
        typename DistFull,
        typename Pred = lib::Returns<lib::Const<true>>>
    void search_patch(
        const data::ConstSimpleDataView<QueryType>& queries,
        const threads::UnitRange<size_t>& data_indices,
        const threads::UnitRange<size_t>& query_indices,
        sorter_type& scratch,
        distance::BroadcastDistance<DistFull>& distance_functors,
        Pred predicate = lib::Returns(lib::Const<true>())
    ) {
        assert(distance_functors.size() >= query_indices.size());

        // Fix arguments
        for (size_t i = 0; i < query_indices.size(); ++i) {
            distance::maybe_fix_argument(
                distance_functors[i], queries.get_datum(query_indices[i], data::full_access)
            );
        }

        for (auto data_index : data_indices) {
            // Skip this index if it doesn't pass the predicate.
            if (!predicate(data_index)) {
                continue;
            }

            auto datum = data_.get_datum(data_index, data::full_access);

            // Loop over the queries.
            // Compute the distance between each query and the dataset element and insert
            // it into the sorting network.
            for (size_t i = 0; i < query_indices.size(); ++i) {
                auto query_index = query_indices[i];
                auto d = distance::compute(
                    distance_functors[i],
                    queries.get_datum(query_index, data::full_access),
                    datum
                );
                scratch.insert(query_index, {data_index, d});
            }
        }
    }

    // Threading Interface

    /// Return whether this implementation can dynamically change the number of threads.
    static bool can_change_threads() { return true; }

    ///
    /// @brief Return the current number of threads used for search.
    ///
    /// @sa set_num_threads
    size_t get_num_threads() const { return threadpool_.size(); }

    ///
    /// @brief Set the number of threads used for search.
    ///
    /// @param num_threads The new number of threads to use.
    ///
    /// Implementation note: The number of threads cannot be zero. If zero is passed to
    /// this method, it will be silently changed to 1.
    ///
    /// @sa get_num_threads
    ///
    void set_num_threads(size_t num_threads) {
        num_threads = std::max(num_threads, size_t(1));
        threadpool_.resize(num_threads);
    }

    // Batchsize API.
    size_t get_data_batch_size() const { return data_batch_size_; }
    void set_data_batch_size(size_t data_batch_size) { data_batch_size_ = data_batch_size; }

    size_t get_query_batch_size() const { return query_batch_size_; }
    void set_query_batch_size(size_t query_batch_size) {
        query_batch_size_ = query_batch_size;
    }
};

/// @brief Forward an existing dataset.
template <data::ImmutableMemoryDataset Data, threads::ThreadPool Pool>
Data&& load_dataset(
    NoopLoaderTag SVS_UNUSED(tag), Data&& data, Pool& /*threadpool*/
) {
    return std::forward<Data>(data);
}

/// @brief Load a standard dataset.
template <typename T, size_t Extent, typename Builder, threads::ThreadPool Pool>
typename VectorDataLoader<T, Extent, Builder>::return_type load_dataset(
    VectorDataLoaderTag SVS_UNUSED(tag),
    const VectorDataLoader<T, Extent, Builder>& loader,
    Pool& /*threadpool*/
) {
    return loader.load();
}

/// @brief Load a compressed dataset.
template <typename Loader, threads::ThreadPool Pool>
auto load_dataset(
    quantization::lvq::CompressorTag SVS_UNUSED(tag), const Loader& loader, Pool& threadpool
) {
    return loader.load(
        svs::data::PolymorphicBuilder<HugepageAllocator>(), threadpool.size()
    );
}

///
/// @class hidden_flat_auto_assemble
///
/// data_loader
/// ===========
///
/// The data loader should be an instance of one of the classes below.
///
/// * An instance of ``VectorDataLoader``.
/// * An LVQ loader: ``svs::quantization::lvq::OneLevelWithBias``.
/// * An implementation of ``svs::data::ImmutableMemoryDataset`` (passed by value).
///
///

///
/// @brief Entry point for loading a Flat index.
///
/// @param data_proto Data prototype. See expanded notes.
/// @param distance The distance **functor** to use to compare queries with elements of the
///     dataset.
/// @param threadpool_proto Precursor for the thread pool to use. Can either be a threadpool
///     instance of an integer specifying the number of threads to use.
///
/// This method provides much of the heavy lifting for constructing a Flat index from
/// a data file on disk or a dataset in memory.
///
/// @copydoc hidden_flat_auto_assemble
///
template <typename DataProto, typename Distance, typename ThreadPoolProto>
auto auto_assemble(
    DataProto&& data_proto, Distance distance, ThreadPoolProto threadpool_proto
) {
    auto threadpool = threads::as_threadpool(threadpool_proto);
    auto data = load_dataset(
        lib::loader_tag<std::decay_t<DataProto>>,
        std::forward<DataProto>(data_proto),
        threadpool
    );
    return FlatIndex(std::move(data), std::move(distance), std::move(threadpool));
}

/// @brief Alias for a short-lived flat index.
template <data::ImmutableMemoryDataset Data, typename Dist>
using TemporaryFlatIndex = FlatIndex<Data, Dist, ReferencesMembers>;

template <data::ImmutableMemoryDataset Data, typename Dist>
TemporaryFlatIndex<Data, Dist>
temporary_flat_index(Data& data, Dist distance, threads::NativeThreadPool& threadpool) {
    return TemporaryFlatIndex<Data, Dist>{data, distance, threadpool};
}

} // namespace svs::index::flat
