// Tests of the folder hash that the setup flow uses to check the generated code (task T-617).
// The trees are built in the test, under the temporary folder; no game file is read.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "tree_hash.h"

namespace fs = std::filesystem;
using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

// A file to create: relative path and contents.
using Entry = std::pair<std::string, std::string>;

static const fs::path kBase = fs::temp_directory_path() / "don-test-tree-hash";

// Creates the files of `entries` under `root`, in the order given.
static void make_tree(const fs::path& root, const std::vector<Entry>& entries) {
    for (const auto& [rel, data] : entries) {
        const fs::path p = root / fs::path(rel);
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out.write(data.data(), std::streamsize(data.size()));
    }
}

// Hash of `root`; the error must be empty, otherwise the test fails.
static std::string hash_of(const fs::path& root) {
    std::string error;
    const std::string h = tree_hash(root, error);
    if (!error.empty()) std::cerr << "unexpected error: " << error << "\n";
    CHECK(error.empty());
    return h;
}

// The tiny reference tree: a.txt, sub/b.bin (with a 0x00 and 0xff byte), and an empty folder that does not count.
static const std::vector<Entry> kReference = {
    {"a.txt", "abc"},
    {"sub/b.bin", std::string("\x00\x01\xff", 3)},
};

int main() {
    fs::remove_all(kBase);
    fs::create_directories(kBase);

    // The reference tree, created in its own order and with an empty folder in it.
    const fs::path ref = kBase / "ref";
    make_tree(ref, kReference);
    fs::create_directories(ref / "empty");
    const std::string ref_hash = hash_of(ref);

    // Known value. Computed once with Python (hashlib, struct) from the byte format of tree_hash.h:
    //   for each file in byte order of its path: path + b"\0" + struct.pack("<Q", size) + contents, all hashed.
    CHECK(ref_hash == "8055d3c1cdf94fa374c51dbcbec299d12e3a29f87b22f59a3849a97e93b13c9b");

    // Same tree, same hash, asked twice.
    CHECK(hash_of(ref) == ref_hash);

    // Same files created in reverse order, under another root: same hash.
    const fs::path reversed = kBase / "reversed";
    make_tree(reversed, {kReference[1], kReference[0]});
    fs::create_directories(reversed / "empty");
    CHECK(hash_of(reversed) == ref_hash);

    // An empty folder alone is an empty tree; it has a hash too, and it differs from the reference.
    const fs::path only_folder = kBase / "only_folder";
    fs::create_directories(only_folder / "empty");
    const std::string empty_hash = hash_of(only_folder);
    CHECK(empty_hash.size() == 64);
    CHECK(empty_hash != ref_hash);

    // One changed byte.
    const fs::path one_byte = kBase / "one_byte";
    make_tree(one_byte, {{"a.txt", "abd"}, kReference[1]});
    CHECK(hash_of(one_byte) != ref_hash);

    // A renamed file.
    const fs::path renamed = kBase / "renamed";
    make_tree(renamed, {{"c.txt", "abc"}, kReference[1]});
    CHECK(hash_of(renamed) != ref_hash);

    // An added file.
    const fs::path added = kBase / "added";
    make_tree(added, {kReference[0], kReference[1], {"d.txt", ""}});
    CHECK(hash_of(added) != ref_hash);

    // A file moved into a subfolder.
    const fs::path moved = kBase / "moved";
    make_tree(moved, {{"sub/a.txt", "abc"}, kReference[1]});
    CHECK(hash_of(moved) != ref_hash);

    // Bytes that move between two files keep the content but not the paths: different hash.
    const fs::path swapped = kBase / "swapped";
    make_tree(swapped, {{"a.txt", std::string("\x00\x01\xff", 3)}, {"sub/b.bin", "abc"}});
    CHECK(hash_of(swapped) != ref_hash);

    // A symbolic link is an error, not followed. Skipped (not failed) where the system cannot create one.
    const fs::path linked = kBase / "linked";
    make_tree(linked, kReference);
    std::error_code link_ec;
    fs::create_symlink(ref / "a.txt", linked / "link.txt", link_ec);
    // (Wine creates the link without error but reports an ordinary file, hence the is_symlink test.)
    if (link_ec || !fs::is_symlink(linked / "link.txt")) {
        std::cerr << "symlink test skipped: " << (link_ec ? link_ec.message() : "the link is not reported as one") << "\n";
    } else {
        std::string error;
        CHECK(tree_hash(linked, error).empty());
        CHECK(error.find("symbolic link") != std::string::npos);
    }

    // A folder that does not exist is an error.
    std::string error;
    CHECK(tree_hash(kBase / "missing", error).empty());
    CHECK(!error.empty());

    fs::remove_all(kBase);
    if (g_fail == 0) std::cout << "test_setup_tree_hash: OK\n";
    return g_fail == 0 ? 0 : 1;
}
