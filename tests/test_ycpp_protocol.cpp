// W4 sync-protocol gate. Two peers do the SV-step1 / diff-step2 handshake
// over the envelope wire and converge.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_byteview.h"
#include "ycpp/ycpp_doc.h"
#include "ycpp/ycpp_envelope.h"
#include "ycpp/ycpp_protocol.h"
#include "ycpp/ycpp_state_vector.h"
#include "ycpp/ycpp_status.h"
#include "ycpp/ycpp_writer.h"

using ycpp::ByteView;
using ycpp::DefaultArenaAllocator;
using ycpp::Envelope;
using ycpp::MessageKind;
using ycpp::Status;
using ycpp::Writer;
using DocA = ycpp::Doc<DefaultArenaAllocator>;
using SvA  = ycpp::StateVector<DefaultArenaAllocator>;

namespace {

ByteView sv(const char* s) noexcept {
    return ByteView{reinterpret_cast<const uint8_t*>(s), std::strlen(s)};
}

std::string map_value(DocA& d, const char* root, const char* key) {
    auto* m = d.get_or_create_map(root);
    if (m == nullptr) return {};
    auto* it = m->get(sv(key));
    if (it == nullptr) return {};
    return {reinterpret_cast<const char*>(it->content_view.data),
            it->content_view.size};
}

} // namespace

TEST(YcppProtocol, Step1FollowedByStep2ConvergesPeers) {
    DocA alice{1};
    DocA bob  {2};
    ASSERT_EQ(alice.map_set_string("doc", "title", "yjs-style RPC"), Status::kOk);
    ASSERT_EQ(alice.map_set_string("doc", "owner", "alice"),         Status::kOk);
    ASSERT_EQ(bob  .map_set_string("doc", "editor", "bob"),          Status::kOk);

    // --- Round-trip 1: Alice sends Step1 (her SV); Bob replies with Step2. ---
    std::array<uint8_t, 4096> wire_a{};
    Writer w_a{wire_a.data(), wire_a.size()};
    DefaultArenaAllocator scratch_a_out;
    ASSERT_EQ(emit_sync_step1(alice, scratch_a_out, /*request_id=*/1, &w_a),
              Status::kOk);

    // Bob decodes Alice's Step1.
    Envelope env{};
    ASSERT_EQ(decode_envelope(ByteView{wire_a.data(), w_a.pos()}, &env), Status::kOk);
    EXPECT_EQ(env.kind, MessageKind::kSyncStep1);

    DefaultArenaAllocator scratch_b;
    SvA alice_sv{&scratch_b};
    ASSERT_EQ(apply_sync_message<DefaultArenaAllocator>(bob, env, &alice_sv),
              Status::kOk);

    // Bob replies with Step2 (the diff Alice is missing) + his own Step1.
    std::array<uint8_t, 4096> wire_b{};
    Writer w_b{wire_b.data(), wire_b.size()};
    ASSERT_EQ(emit_sync_step2(bob, &alice_sv, /*request_id=*/1, &w_b),
              Status::kOk);

    // Alice consumes Bob's Step2 — gets Bob's "editor".
    Envelope env2{};
    ASSERT_EQ(decode_envelope(ByteView{wire_b.data(), w_b.pos()}, &env2), Status::kOk);
    EXPECT_EQ(env2.kind, MessageKind::kSyncStep2);
    ASSERT_EQ(apply_sync_message<DefaultArenaAllocator>(alice, env2,
                                                        /*peer_sv_out=*/nullptr),
              Status::kOk);
    EXPECT_EQ(map_value(alice, "doc", "editor"), std::string{"bob"});

    // --- Round-trip 2: Bob does Step1 → Alice replies with Step2. ---
    std::array<uint8_t, 4096> wire_c{};
    Writer w_c{wire_c.data(), wire_c.size()};
    DefaultArenaAllocator scratch_b_out;
    ASSERT_EQ(emit_sync_step1(bob, scratch_b_out, /*request_id=*/2, &w_c),
              Status::kOk);

    Envelope env3{};
    ASSERT_EQ(decode_envelope(ByteView{wire_c.data(), w_c.pos()}, &env3), Status::kOk);
    DefaultArenaAllocator scratch_a;
    SvA bob_sv{&scratch_a};
    ASSERT_EQ(apply_sync_message<DefaultArenaAllocator>(alice, env3, &bob_sv),
              Status::kOk);

    std::array<uint8_t, 4096> wire_d{};
    Writer w_d{wire_d.data(), wire_d.size()};
    ASSERT_EQ(emit_sync_step2(alice, &bob_sv, /*request_id=*/2, &w_d), Status::kOk);
    Envelope env4{};
    ASSERT_EQ(decode_envelope(ByteView{wire_d.data(), w_d.pos()}, &env4), Status::kOk);
    ASSERT_EQ(apply_sync_message<DefaultArenaAllocator>(bob, env4, nullptr),
              Status::kOk);

    EXPECT_EQ(map_value(bob, "doc", "title"), std::string{"yjs-style RPC"});
    EXPECT_EQ(map_value(bob, "doc", "owner"), std::string{"alice"});
}
