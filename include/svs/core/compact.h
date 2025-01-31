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

// svs
#include "svs/concepts/data.h"
#include "svs/core/data/simple.h"
#include "svs/lib/threads.h"
#include "svs/third-party/fmt.h"

// tsl
#include "tsl/robin_map.h"

// stl
#include <vector>

namespace svs {

template <
    data::MemoryDataset Data,
    data::MemoryDataset Buffer,
    typename I,
    typename Alloc,
    threads::ThreadPool Pool>
void compact_data(
    Data& data, Buffer& buffer, const std::vector<I, Alloc>& new_to_old, Pool& threadpool
) {
    // The contents of the data and the buffer should be the same.
    static_assert(std::is_same_v<data::value_type_t<Data>, data::value_type_t<Buffer>>);
    assert(std::is_sorted(new_to_old.begin(), new_to_old.end()));

    auto data_dims = data.dimensions();
    auto buffer_dims = buffer.dimensions();
    if (data_dims != buffer_dims) {
        auto msg = fmt::format(
            "Data dims ({}) does not match buffer dims ({})", data_dims, buffer_dims
        );
        throw ANNEXCEPTION(msg);
    }

    // The batchsize is the amount of room we have in the temporary buffer.
    size_t batchsize = buffer.size();
    size_t start = 0;
    size_t end = new_to_old.size();
    while (start < end) {
        size_t stop = std::min(start + batchsize, end);
        auto batch_to_new = threads::UnitRange{start, stop};
        auto this_batch = batch_to_new.eachindex();

        // Copy from the original dataset into the buffer.
        threads::run(
            threadpool,
            threads::StaticPartition(this_batch),
            [&](const auto& batch_ids, uint64_t SVS_UNUSED(tid)) {
                for (auto batch_id : batch_ids) {
                    auto old_id = new_to_old[batch_to_new[batch_id]];
                    buffer.set_datum(batch_id, data.get_datum(old_id));
                }
            }
        );

        // Copy back from the buffer to the new location in the original dataset.
        threads::run(
            threadpool,
            threads::StaticPartition(this_batch),
            [&](const auto& batch_ids, uint64_t SVS_UNUSED(tid)) {
                for (auto batch_id : batch_ids) {
                    auto new_id = batch_to_new[batch_id];
                    data.set_datum(new_id, buffer.get_datum(batch_id));
                }
            }
        );
        start = stop;
    }
}

} // namespace svs
