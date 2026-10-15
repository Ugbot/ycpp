// W3 convergence gate. Two Docs make independent edits, exchange
// state-vector-bounded diffs in both directions, end up with identical
// observable state.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_byteview.h"
#include "ycpp/ycpp_doc.h"
#include "ycpp/ycpp_state_vector.h"
#include "ycpp/ycpp_status.h"
#include "ycpp/ycpp_writer.h"

using ycpp::ByteView;
using ycpp::DefaultArenaAllocator;
using ycpp::Status;
using ycpp::Writer;
using DocA = ycpp::Doc<DefaultArenaAllocator>;
using SvA  = ycpp::StateVector<DefaultArenaAllocator>;

namespace {

ByteView sv(const char* s) noexcept {
    return ByteView{reinterpret_cast<const uint8_t*>(s), std::strlen(s)};
}
std::string_view as_sv(ByteView b) noexcept {
    return {reinterpret_cast<const char*>(b.data), b.size};
}

// One-shot sync helper: emits A's diff since B's state vector and
// applies it to B. Returns Status::kOk on success.
Status sync(const DocA& src, DocA& dst) noexcept {
    DefaultArenaAllocator a;
    SvA dst_sv{&a};
    if (Status s = dst.state_vector(&dst_sv); s != Status::kOk) return s;

    std::array<uint8_t, 4096> buf{};
    Writer w{buf.data(), buf.size()};
    if (Status s = encode_diff_v1<DefaultArenaAllocator>(src, &dst_sv, &w); s != Status::kOk) return s;
    return dst.apply_update_v1(ByteView{buf.data(), w.pos()});
}

} // namespace

TEST(YcppConvergence, BidirectionalSyncReachesSameState) {
    DocA alice{1};
    DocA bob  {2};

    // Independent edits on disjoint keys — no Lamport conflicts.
    ASSERT_EQ(alice.map_set_string("doc", "title",   "Hello, world"), Status::kOk);
    ASSERT_EQ(alice.map_set_string("doc", "author",  "alice"),        Status::kOk);
    ASSERT_EQ(bob  .map_set_string("doc", "editor",  "vim"),          Status::kOk);
    ASSERT_EQ(bob  .map_set_string("doc", "version", "0.1"),          Status::kOk);

    // Exchange diffs both ways.
    ASSERT_EQ(sync(alice, bob),   Status::kOk);
    ASSERT_EQ(sync(bob,   alice), Status::kOk);

    for (DocA* d : {&alice, &bob}) {
        auto* m = d->get_or_create_map("doc");
        ASSERT_NE(m, nullptr);
        ASSERT_NE(m->get(sv("title")),   nullptr);
        ASSERT_NE(m->get(sv("author")),  nullptr);
        ASSERT_NE(m->get(sv("editor")),  nullptr);
        ASSERT_NE(m->get(sv("version")), nullptr);
        EXPECT_EQ(as_sv(m->get(sv("title"))  ->content_view), std::string_view{"Hello, world"});
        EXPECT_EQ(as_sv(m->get(sv("author")) ->content_view), std::string_view{"alice"});
        EXPECT_EQ(as_sv(m->get(sv("editor")) ->content_view), std::string_view{"vim"});
        EXPECT_EQ(as_sv(m->get(sv("version"))->content_view), std::string_view{"0.1"});
    }
}

TEST(YcppConvergence, ConcurrentEditsToSameKeyResolveByLamport) {
    // Two clients write to the same key without first seeing each other.
    // Higher (client, clock) Id wins; the loser is marked deleted on
    // both peers after the diffs converge.
    DocA low {1};   // smaller client id
    DocA high{99};  // larger client id

    ASSERT_EQ(low .map_set_string("r", "k", "from-low"),  Status::kOk);  // (1, 0)
    ASSERT_EQ(high.map_set_string("r", "k", "from-high"), Status::kOk);  // (99, 0)

    ASSERT_EQ(sync(low,  high), Status::kOk);
    ASSERT_EQ(sync(high, low),  Status::kOk);

    for (DocA* d : {&low, &high}) {
        auto* m = d->get_or_create_map("r");
        ASSERT_NE(m, nullptr);
        auto* head = m->get(sv("k"));
        ASSERT_NE(head, nullptr);
        EXPECT_EQ(as_sv(head->content_view), std::string_view{"from-high"});
        EXPECT_TRUE(d->delete_set().contains(ycpp::Id{1, 0}));
    }
}

TEST(YcppConvergence, MultiRoundSyncStaysConvergent) {
    DocA alice{1};
    DocA bob  {2};

    for (int round = 0; round < 5; ++round) {
        char ka[8]; std::snprintf(ka, sizeof(ka), "a%d", round);
        char kb[8]; std::snprintf(kb, sizeof(kb), "b%d", round);
        ASSERT_EQ(alice.map_set_string("r", ka, "alpha"), Status::kOk);
        ASSERT_EQ(bob  .map_set_string("r", kb, "beta"),  Status::kOk);
        ASSERT_EQ(sync(alice, bob),   Status::kOk);
        ASSERT_EQ(sync(bob,   alice), Status::kOk);
    }

    for (DocA* d : {&alice, &bob}) {
        auto* m = d->get_or_create_map("r");
        ASSERT_NE(m, nullptr);
        for (int round = 0; round < 5; ++round) {
            char ka[8]; std::snprintf(ka, sizeof(ka), "a%d", round);
            char kb[8]; std::snprintf(kb, sizeof(kb), "b%d", round);
            EXPECT_NE(m->get(sv(ka)), nullptr) << "round=" << round;
            EXPECT_NE(m->get(sv(kb)), nullptr) << "round=" << round;
        }
    }
}

namespace {

std::string_view head_of(DocA& d, const char* root, const char* key) noexcept {
    auto* m = d.get_or_create_map(root);
    if (m == nullptr) return {};
    const ycpp::Item* h = m->get(sv(key));
    return h == nullptr ? std::string_view{} : as_sv(h->content_view);
}

} // namespace

// G2CHK-136: a client that has observed another client's value and then
// overwrites it must win, regardless of which client id is larger.
TEST(YcppConvergence, CausalOverwriteByLowerClientWins) {
    DocA low {0x11};
    DocA high{0x22};

    ASSERT_EQ(high.map_set_string("members", "22", "status=alive"), Status::kOk);
    ASSERT_EQ(sync(high, low), Status::kOk);
    ASSERT_EQ(head_of(low, "members", "22"), std::string_view{"status=alive"});

    ASSERT_EQ(low.map_set_string("members", "22", "status=dead"), Status::kOk);
    EXPECT_EQ(head_of(low, "members", "22"), std::string_view{"status=dead"});

    // Idempotent: a second overwrite stays visible.
    ASSERT_EQ(low.map_set_string("members", "22", "status=dead;reaped=1"), Status::kOk);
    EXPECT_EQ(head_of(low, "members", "22"), std::string_view{"status=dead;reaped=1"});

    ASSERT_EQ(sync(low, high), Status::kOk);
    EXPECT_EQ(head_of(high, "members", "22"), std::string_view{"status=dead;reaped=1"});
    for (DocA* d : {&low, &high}) {
        EXPECT_TRUE(d->delete_set().contains(ycpp::Id{0x22, 0}));
    }
}

// Concurrent overwrites of a shared base converge on the branch rooted at
// the larger-client child of the common ancestor (YATA order), even when
// the other branch is longer.
TEST(YcppConvergence, DivergentOverwriteBranchesConverge) {
    DocA base{5};
    DocA a{1};
    DocA b{9};

    ASSERT_EQ(base.map_set_string("r", "k", "x"), Status::kOk);
    ASSERT_EQ(sync(base, a), Status::kOk);
    ASSERT_EQ(sync(base, b), Status::kOk);

    ASSERT_EQ(a.map_set_string("r", "k", "a1"), Status::kOk);
    ASSERT_EQ(a.map_set_string("r", "k", "a2"), Status::kOk);
    ASSERT_EQ(b.map_set_string("r", "k", "b1"), Status::kOk);

    ASSERT_EQ(sync(a, b), Status::kOk);
    ASSERT_EQ(sync(b, a), Status::kOk);
    ASSERT_EQ(sync(a, base), Status::kOk);
    ASSERT_EQ(sync(b, base), Status::kOk);

    for (DocA* d : {&a, &b, &base}) {
        EXPECT_EQ(head_of(*d, "r", "k"), std::string_view{"b1"});
    }

    // A later causal overwrite on the losing side wins everywhere.
    ASSERT_EQ(a.map_set_string("r", "k", "a3"), Status::kOk);
    ASSERT_EQ(sync(a, b), Status::kOk);
    ASSERT_EQ(sync(a, base), Status::kOk);
    for (DocA* d : {&a, &b, &base}) {
        EXPECT_EQ(head_of(*d, "r", "k"), std::string_view{"a3"});
    }
}

// A late concurrent overwrite must converge while compaction retains the
// complete origin chain needed by existing and newly arriving map values.
TEST(YcppConvergence, LateOverwriteRetainsOriginAnchorsAndKeepsKey) {
    DocA base{5};
    DocA a{1};
    DocA c{3};

    ASSERT_EQ(base.map_set_string("r", "k", "x"), Status::kOk);
    ASSERT_EQ(sync(base, a), Status::kOk);
    ASSERT_EQ(sync(base, c), Status::kOk);

    ASSERT_EQ(a.map_set_string("r", "k", "a1"), Status::kOk);
    ASSERT_EQ(a.map_set_string("r", "k", "a2"), Status::kOk);
    ASSERT_EQ(c.map_set_string("r", "k", "c1"), Status::kOk);

    ASSERT_EQ(sync(a, base), Status::kOk);
    // a2 references the last clock of the deleted a1 string; a1 in turn
    // references x. Both anchors must survive, even though both are deleted.
    EXPECT_EQ(base.compact(), 0U);
    ASSERT_EQ(sync(c, base), Status::kOk);
    EXPECT_EQ(head_of(base, "r", "k"), std::string_view{"c1"});

    ASSERT_EQ(sync(a, c), Status::kOk);
    ASSERT_EQ(sync(c, a), Status::kOk);
    EXPECT_EQ(head_of(a, "r", "k"), std::string_view{"c1"});
    EXPECT_EQ(head_of(c, "r", "k"), std::string_view{"c1"});
}
