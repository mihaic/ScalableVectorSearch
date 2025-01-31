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

#include "svs/core/data/abstract_io.h"
#include "svs/core/data/block.h"
#include "svs/core/data/simple.h"
#include "svs/core/io.h"

#include "svs/lib/array.h"
#include "svs/lib/exception.h"
#include "svs/lib/memory.h"
#include "svs/lib/meta.h"
#include "svs/lib/misc.h"

namespace svs::io {

namespace detail {
inline void static_size_check(size_t expected, size_t actual) {
    if (expected != actual) {
        throw ANNEXCEPTION(
            "Trying to populate a dataset with static extent ",
            expected,
            " while the loaded dataset has dimension ",
            actual,
            "!"
        );
    }
}
} // namespace detail

/////
///// Dataset Loading
/////

// Generic dataset loading.
template <typename T, size_t Extent, typename File, typename Builder>
typename Builder::template return_type<T, Extent>
load_impl(const File& file, const Builder& builder) {
    auto [vectors_to_read, ndims] = file.get_dims();

    // Size check to throw an error early.
    if constexpr (Extent != Dynamic) {
        detail::static_size_check(Extent, ndims);
    }

    auto data = data::build<T, Extent>(builder, vectors_to_read, ndims);
    populate(data, file);
    return data;
}

namespace detail {
// Promote untyped string-like arguments to a `NativeFile`.
template <typename T> const T& to_native(const T& x) { return x; }
inline NativeFile to_native(const std::string& x) { return NativeFile(x); }
inline NativeFile to_native(const std::string_view& x) { return to_native(std::string(x)); }
inline NativeFile to_native(const std::filesystem::path& x) { return NativeFile(x); }
} // namespace detail

template <
    typename T,
    size_t Extent,
    typename File,
    typename Builder = data::PolymorphicBuilder<>>
auto load_dataset(const File& file, const Builder& builder = data::PolymorphicBuilder<>{}) {
    return load_impl<T, Extent>(detail::to_native(file), builder);
}

///
/// @brief Load a dataset from file. Automcatically detect the file type based on extension.
///
/// @tparam T The element type of the vector components in the file.
/// @tparam Extent The compile-time dimensionality of the dataset to load. This will be
///     check by the actual loading mechanism if it's able to.
/// @tparam Allocator The allocator type to use for the resulting dataset.
///
/// @param filename The path to the file on disk.
/// @param allocator The allocator instance to use for allocation.
///
/// Recognized file extentions:
/// * .svs: The native file format for this library.
/// * .vecs: The usual [f/b/i]vecs form.
/// * .bin: Files generated by DiskANN.
///
template <
    typename T,
    size_t Extent = Dynamic,
    typename Builder = data::PolymorphicBuilder<>>
auto auto_load(
    const std::string& filename, const Builder& builder = data::PolymorphicBuilder<>{}
) {
    if (filename.ends_with("svs")) {
        return load_dataset<T, Extent>(io::NativeFile(filename), builder);
    } else if (filename.ends_with("vecs")) {
        return load_dataset<T, Extent>(io::vecs::VecsFile<T>(filename), builder);
    } else if (filename.ends_with("bin")) {
        return load_dataset<T, Extent>(io::binary::BinaryFile(filename), builder);
    } else {
        throw ANNEXCEPTION("Unknown file extension for input file: ", filename, ".");
    }
}

} // namespace svs::io
