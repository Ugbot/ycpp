// ycpp_reader.h — pointer-bumping binary reader over a ByteView.
//
// Returns Status on every fallible operation. No exceptions, no allocations.
// Bounds are checked once per primitive — the caller never reads past the
// view, but never has to compute the offset arithmetic either.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ycpp_byteview.h"
#include "ycpp_status.h"
#include "ycpp_varint.h"

namespace ycpp {

class Reader {
public:
    constexpr Reader() noexcept : view_{}, pos_(0) {}
    explicit constexpr Reader(ByteView v) noexcept : view_(v), pos_(0) {}

    [[nodiscard]] constexpr size_t pos      () const noexcept { return pos_; }
    [[nodiscard]] constexpr size_t remaining() const noexcept {
        assert(pos_ <= view_.size);
        return view_.size - pos_;
    }
    [[nodiscard]] constexpr bool   eof      () const noexcept {
        return pos_ >= view_.size;
    }
    [[nodiscard]] constexpr ByteView tail   () const noexcept {
        assert(pos_ <= view_.size);
        return view_.drop_front(pos_);
    }

    [[nodiscard]] Status u8(uint8_t* out) noexcept {
        assert(out != nullptr);
        if (remaining() < 1) return Status::kOutOfBounds;
        *out = view_.data[pos_++];
        return Status::kOk;
    }

    [[nodiscard]] Status u16_le(uint16_t* out) noexcept {
        assert(out != nullptr);
        if (remaining() < 2) return Status::kOutOfBounds;
        uint16_t v = 0;
        std::memcpy(&v, view_.data + pos_, 2);
        pos_ += 2;
        *out = v;
        return Status::kOk;
    }

    [[nodiscard]] Status u32_le(uint32_t* out) noexcept {
        assert(out != nullptr);
        if (remaining() < 4) return Status::kOutOfBounds;
        uint32_t v = 0;
        std::memcpy(&v, view_.data + pos_, 4);
        pos_ += 4;
        *out = v;
        return Status::kOk;
    }

    [[nodiscard]] Status u64_le(uint64_t* out) noexcept {
        assert(out != nullptr);
        if (remaining() < 8) return Status::kOutOfBounds;
        uint64_t v = 0;
        std::memcpy(&v, view_.data + pos_, 8);
        pos_ += 8;
        *out = v;
        return Status::kOk;
    }

    [[nodiscard]] Status varint_u64(uint64_t* out) noexcept {
        assert(out != nullptr);
        size_t consumed = 0;
        const Status s = varint_decode_u64(view_.data + pos_, remaining(),
                                           out, &consumed);
        if (s != Status::kOk) return s;
        pos_ += consumed;
        return Status::kOk;
    }

    [[nodiscard]] Status varint_i64(int64_t* out) noexcept {
        assert(out != nullptr);
        size_t consumed = 0;
        const Status s = varint_decode_i64(view_.data + pos_, remaining(),
                                           out, &consumed);
        if (s != Status::kOk) return s;
        pos_ += consumed;
        return Status::kOk;
    }

    // Reads a length-prefixed byte run (varint-u64 length, then `len` bytes).
    // The returned view aliases the underlying buffer — callers must not
    // outlive the Reader's source.
    [[nodiscard]] Status length_prefixed(ByteView* out) noexcept {
        assert(out != nullptr);
        uint64_t len = 0;
        YCPP_TRY(varint_u64(&len));
        if (len > remaining()) return Status::kOutOfBounds;
        *out = ByteView{ view_.data + pos_, static_cast<size_t>(len) };
        pos_ += static_cast<size_t>(len);
        return Status::kOk;
    }

    [[nodiscard]] Status skip(size_t n) noexcept {
        if (n > remaining()) return Status::kOutOfBounds;
        pos_ += n;
        return Status::kOk;
    }

private:
    ByteView view_;
    size_t   pos_;
};

} // namespace ycpp
