// ycpp_writer.h — pointer-bumping binary writer over a caller-owned buffer.
//
// No heap; the caller supplies the destination. Out-of-capacity surfaces as
// `Status::kOutOfBounds`. Wave-9 production encoders pair this with an
// arena-backed extensible buffer; the writer itself stays buffer-agnostic.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ycpp_status.h"
#include "ycpp_varint.h"

namespace ycpp {

class Writer {
public:
    constexpr Writer() noexcept : buf_(nullptr), cap_(0), pos_(0) {}
    Writer(uint8_t* buf, size_t cap) noexcept : buf_(buf), cap_(cap), pos_(0) {
        assert((buf != nullptr) || (cap == 0));
    }

    [[nodiscard]] constexpr size_t pos      () const noexcept { return pos_; }
    [[nodiscard]] constexpr size_t capacity () const noexcept { return cap_; }
    [[nodiscard]] constexpr size_t remaining() const noexcept {
        assert(pos_ <= cap_);
        return cap_ - pos_;
    }
    [[nodiscard]] uint8_t* data() noexcept { return buf_; }

    [[nodiscard]] Status u8(uint8_t v) noexcept {
        if (remaining() < 1) return Status::kOutOfBounds;
        assert(buf_ != nullptr);
        buf_[pos_++] = v;
        return Status::kOk;
    }

    [[nodiscard]] Status u16_le(uint16_t v) noexcept {
        if (remaining() < 2) return Status::kOutOfBounds;
        assert(buf_ != nullptr);
        std::memcpy(buf_ + pos_, &v, 2);
        pos_ += 2;
        return Status::kOk;
    }

    [[nodiscard]] Status u32_le(uint32_t v) noexcept {
        if (remaining() < 4) return Status::kOutOfBounds;
        assert(buf_ != nullptr);
        std::memcpy(buf_ + pos_, &v, 4);
        pos_ += 4;
        return Status::kOk;
    }

    [[nodiscard]] Status u64_le(uint64_t v) noexcept {
        if (remaining() < 8) return Status::kOutOfBounds;
        assert(buf_ != nullptr);
        std::memcpy(buf_ + pos_, &v, 8);
        pos_ += 8;
        return Status::kOk;
    }

    [[nodiscard]] Status varint_u64(uint64_t v) noexcept {
        size_t w = 0;
        const Status s = varint_encode_u64(v, buf_ + pos_, remaining(), &w);
        if (s != Status::kOk) return s;
        pos_ += w;
        return Status::kOk;
    }

    [[nodiscard]] Status varint_i64(int64_t v) noexcept {
        size_t w = 0;
        const Status s = varint_encode_i64(v, buf_ + pos_, remaining(), &w);
        if (s != Status::kOk) return s;
        pos_ += w;
        return Status::kOk;
    }

    [[nodiscard]] Status bytes(const uint8_t* src, size_t n) noexcept {
        assert(src != nullptr || n == 0);
        if (n > remaining()) return Status::kOutOfBounds;
        if (n != 0) {
            assert(buf_ != nullptr);
            std::memcpy(buf_ + pos_, src, n);
        }
        pos_ += n;
        return Status::kOk;
    }

    // Writes a length-prefixed byte run: varint-u64 length followed by `n` bytes.
    [[nodiscard]] Status length_prefixed(const uint8_t* src, size_t n) noexcept {
        assert(src != nullptr || n == 0);
        YCPP_TRY(varint_u64(static_cast<uint64_t>(n)));
        return bytes(src, n);
    }

private:
    uint8_t* buf_;
    size_t   cap_;
    size_t   pos_;
};

} // namespace ycpp
