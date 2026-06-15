// ycpp_cli — Yjs interop CLI.
//
// Subcommands:
//
//   apply-and-emit
//       Read a Yjs updateV1 blob from stdin. Apply it to a fresh
//       ycpp::Doc. Encode the doc state as updateV1 to stdout (i.e.
//       round-trip: in → store → out). Exit 0 on success.
//
//   list-map  <root_name> <key>
//       Read updateV1 from stdin. Apply. Look up `key` in root Y.Map
//       `root_name`. Print the value bytes to stdout. Exit 0 if found,
//       2 if not.
//
//   dump-text <root_name>
//       Read updateV1 from stdin. Apply. Print the concatenated text
//       of root Y.Text `root_name` to stdout. Always exits 0 if apply
//       succeeded.
//
// On any decode / apply error, exits 1 with a ycpp::Status name on stderr.
//
// I/O is binary on stdin / stdout. The CLI sets stdin/stdout to binary
// mode on Windows so updateV1 bytes survive intact.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
  #include <fcntl.h>
  #include <io.h>
#endif

#include "ycpp/ycpp.h"

namespace {

using DocA = ycpp::Doc<ycpp::DefaultArenaAllocator>;
using SvA  = ycpp::StateVector<ycpp::DefaultArenaAllocator>;

void set_binary_io() noexcept {
#if defined(_WIN32)
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

// Read all of stdin into `out`. Returns true on success.
bool read_all_stdin(std::vector<uint8_t>* out) noexcept {
    uint8_t chunk[4096];
    while (true) {
        std::size_t n = std::fread(chunk, 1, sizeof(chunk), stdin);
        if (n > 0) out->insert(out->end(), chunk, chunk + n);
        if (n < sizeof(chunk)) {
            if (std::feof(stdin)) return true;
            if (std::ferror(stdin)) {
                std::fprintf(stderr, "ycpp_cli: stdin read error\n");
                return false;
            }
        }
    }
}

int do_apply_and_emit(int argc, char** argv) {
    (void)argc; (void)argv;
    std::vector<uint8_t> in;
    if (!read_all_stdin(&in)) return 1;

    DocA doc{/*client_id=*/0xC0FFEE};
    const ycpp::Status s = doc.apply_update_v1(
        ycpp::ByteView{in.data(), in.size()});
    if (s != ycpp::Status::kOk) {
        std::fprintf(stderr, "ycpp_cli: apply failed: %s\n",
                     ycpp::status_name(s));
        return 1;
    }
    // Encode the full state (since empty SV).
    std::vector<uint8_t> out(1 << 20);
    ycpp::Writer w{out.data(), out.size()};
    const ycpp::Status es = ycpp::encode_diff_v1<ycpp::DefaultArenaAllocator>(
        doc, nullptr, &w);
    if (es != ycpp::Status::kOk) {
        std::fprintf(stderr, "ycpp_cli: encode failed: %s\n",
                     ycpp::status_name(es));
        return 1;
    }
    std::fwrite(out.data(), 1, w.pos(), stdout);
    return 0;
}

int do_list_map(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: ycpp_cli list-map <root> <key>\n");
        return 1;
    }
    const char* root = argv[2];
    const char* key  = argv[3];

    std::vector<uint8_t> in;
    if (!read_all_stdin(&in)) return 1;

    DocA doc{0xC0FFEE};
    const ycpp::Status s = doc.apply_update_v1(
        ycpp::ByteView{in.data(), in.size()});
    if (s != ycpp::Status::kOk) {
        std::fprintf(stderr, "ycpp_cli: apply failed: %s\n",
                     ycpp::status_name(s));
        return 1;
    }
    auto* m = doc.get_or_create_map(root);
    if (m == nullptr) {
        std::fprintf(stderr, "ycpp_cli: no map root '%s'\n", root);
        return 2;
    }
    const ycpp::ByteView key_v{reinterpret_cast<const uint8_t*>(key),
                                std::strlen(key)};
    auto* it = m->get(key_v);
    if (it == nullptr) {
        std::fprintf(stderr, "ycpp_cli: key '%s' not present\n", key);
        return 2;
    }
    std::fwrite(it->content_view.data, 1, it->content_view.size, stdout);
    return 0;
}

int do_dump_text(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ycpp_cli dump-text <root>\n");
        return 1;
    }
    const char* root = argv[2];

    std::vector<uint8_t> in;
    if (!read_all_stdin(&in)) return 1;

    DocA doc{0xC0FFEE};
    const ycpp::Status s = doc.apply_update_v1(
        ycpp::ByteView{in.data(), in.size()});
    if (s != ycpp::Status::kOk) {
        std::fprintf(stderr, "ycpp_cli: apply failed: %s\n",
                     ycpp::status_name(s));
        return 1;
    }
    auto text = doc.get_or_create_text(root);
    text.for_each_chunk([](ycpp::ByteView b) noexcept {
        if (b.size > 0) std::fwrite(b.data, 1, b.size, stdout);
    });
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    set_binary_io();
    if (argc < 2) {
        std::fprintf(stderr, "usage: ycpp_cli <apply-and-emit|list-map|dump-text> [args]\n");
        return 1;
    }
    const char* cmd = argv[1];
    if (std::strcmp(cmd, "apply-and-emit") == 0) return do_apply_and_emit(argc, argv);
    if (std::strcmp(cmd, "list-map"      ) == 0) return do_list_map      (argc, argv);
    if (std::strcmp(cmd, "dump-text"     ) == 0) return do_dump_text     (argc, argv);
    std::fprintf(stderr, "ycpp_cli: unknown command '%s'\n", cmd);
    return 1;
}
