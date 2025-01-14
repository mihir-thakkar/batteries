// Copyright 2021 Anthony Paul Astolfi
//
#pragma once
#ifndef BATTERIES_POINTERS_HPP
#define BATTERIES_POINTERS_HPP

#include <memory>

namespace batt {

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
//
struct NoopDeleter {
    template <typename T>
    void operator()(T*) const
    {  // do nothing
    }
};

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
//
template <typename T>
using UniqueNonOwningPtr = std::unique_ptr<T, NoopDeleter>;

}  // namespace batt

#endif  // BATTERIES_POINTERS_HPP
