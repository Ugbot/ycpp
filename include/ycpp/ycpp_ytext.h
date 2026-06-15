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

    // ---- delta() — Yjs-style {insert, attributes} sequence ------------
    //
    // Walks the array in document order; for each live text chunk emits
    // an `Insert` entry carrying the text bytes plus the list of
    // currently-active format Items (their raw key+value-JSON byte
    // payloads). Format Items along the way appear as `Format` entries
    // so the caller can interpret attribute changes.
    //
    // The format payloads are passed verbatim — ycpp doesn't link a
    // JSON decoder. Callers that want a structured `{ key: value }`
    // attribute map parse the lib0/any payload themselves.

    enum class DeltaKind : uint8_t { kInsert, kFormat };

    struct DeltaEntry {
        DeltaKind             kind;
        ByteView              text;             // Insert: text bytes
        const ByteView*       active_formats;   // Insert: array of active format payloads
        std::size_t           active_format_n;
        ByteView              format_payload;   // Format: raw key+value-JSON bytes
        bool                  format_is_deleted;
    };

    template <class Fn>
    void delta(Fn&& on_entry) const noexcept {
        constexpr std::size_t kMaxActiveFormats = 32;
        ByteView active[kMaxActiveFormats];
        std::size_t active_n = 0;

        for (const Item* it = arr_->raw_start(); it != nullptr; it = it->right) {
            if (it->content_kind == ContentKind::kFormat) {
                const bool deleted = (it->flags & kFlagDeleted) != 0;
                DeltaEntry e{};
                e.kind              = DeltaKind::kFormat;
                e.format_payload    = it->content_view;
                e.format_is_deleted = deleted;
                on_entry(e);
                if (!deleted && active_n < kMaxActiveFormats) {
                    active[active_n++] = it->content_view;
                }
                continue;
            }
            if ((it->flags & kFlagDeleted) != 0) continue;
            if (it->content_view.size == 0) continue;
            DeltaEntry e{};
            e.kind             = DeltaKind::kInsert;
            e.text             = it->content_view;
            e.active_formats   = active;
            e.active_format_n  = active_n;
            on_entry(e);
        }
    }

private:
    YArray<A>* arr_;
};

} // namespace ycpp
