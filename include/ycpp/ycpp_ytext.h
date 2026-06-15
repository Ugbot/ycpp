// ycpp_ytext.h — text-friendly facade over YArray.
//
// Y.Text is the collaborative-editing surface: each YArray Item carries
// a UTF-8 byte run as its `content_view`. The facade adds helpers for
// reading the concatenated text and for measuring lengths in bytes vs
// items (one item per char-run today; see LIMITATIONS for character vs
// byte-vs-codepoint semantics).
//
// The facade is value-typed: handing a `YText<A>{ array_ptr }` around is
// cheap (one pointer). The underlying YArray lives in the Doc and owns
// the items.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_arena.h"
#include "ycpp_byteview.h"
#include "ycpp_item.h"
#include "ycpp_status.h"
#include "ycpp_writer.h"
#include "ycpp_yarray.h"

namespace ycpp {

template <Allocator A>
class YText {
public:
    explicit YText(YArray<A>* arr) noexcept : arr_(arr) {
        assert(arr_ != nullptr);
    }

    [[nodiscard]] YArray<A>*       array()       noexcept { return arr_; }
    [[nodiscard]] const YArray<A>* array() const noexcept { return arr_; }

    // # of live items (== chunks of text), NOT bytes / chars.
    [[nodiscard]] uint64_t length() const noexcept { return arr_->length(); }

    // Total live-text byte length (sum of content_view.size over live items).
    [[nodiscard]] std::size_t byte_length() const noexcept {
        std::size_t n = 0;
        arr_->for_each([&](const Item* it, uint64_t /*idx*/) noexcept {
            n += it->content_view.size;
        });
        return n;
    }

    // Write the concatenated text bytes into `w`. Useful for shipping the
    // document text out of the CRDT — for rendering, indexing, etc.
    [[nodiscard]] Status copy_into(Writer& w) const noexcept {
        Status acc = Status::kOk;
        arr_->for_each([&](const Item* it, uint64_t /*idx*/) noexcept {
            if (acc != Status::kOk) return;
            if (it->content_view.size == 0) return;
            const Status s = w.bytes(it->content_view.data, it->content_view.size);
            if (s != Status::kOk) acc = s;
        });
        return acc;
    }

    // Visit every live chunk in document order. `fn` receives the
    // chunk's `ByteView` payload.
    template <class Fn>
    void for_each_chunk(Fn&& fn) const noexcept {
        arr_->for_each([&](const Item* it, uint64_t /*idx*/) noexcept {
            fn(it->content_view);
        });
    }

private:
    YArray<A>* arr_;
};

} // namespace ycpp
