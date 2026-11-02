// Stages of the front half of prepare (T-691) against fakes: no game file is read, the recompiler is a shell script.
#include <algorithm>
#include <vector>
#include <fstream>
#include <iostream>
#include <iterator>

#include "disc_image.h"
#include "module_front.h"
#include "setup_types.h"
#include "tree_hash.h"

using namespace don_setup;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static const fs::path kBase = fs::temp_directory_path() / "don-test-module-front";

static void put32(std::string& s, size_t at, uint32_t v) {
    for (int k = 0; k < 4; ++k) s[at + k] = char(uint8_t(v >> (8 * k)));
}

// A minimal IRX: one PROGBITS section (address 0, 16 bytes) and a section table at 0x100. Synthetic, not game code.
static std::string synthetic_irx() {
    std::string irx(0x200, '\0');
    put32(irx, 32, 0x100);
    irx[46] = 40;
    irx[48] = 2;
    put32(irx, 0x100 + 40 + 4, 1);      // section 1: type PROGBITS
    put32(irx, 0x100 + 40 + 16, 0x80);  // file offset
    put32(irx, 0x100 + 40 + 20, 16);    // size
    put32(irx, 0x80, 0x24020001);       // addiu v0,zero,1
    return irx;
}

// A little-endian FPK with one stored entry named like the game's.
static std::string synthetic_pack(const std::string& data) {
    std::string p(0x10 + 0x30, '\0');
    put32(p, 4, 1);
    put32(p, 8, 0x10);
    put32(p, 12, uint32_t(0x40 + data.size()));
    const std::string name = "MODULES/CRI_ADXI.IRX";
    p.replace(0x10, name.size(), name);
    put32(p, 0x10 + 0x24, 0x40);
    put32(p, 0x10 + 0x28, uint32_t(data.size()));
    put32(p, 0x10 + 0x2C, uint32_t(data.size()));
    return p + data;
}

static void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A recompiler that writes gen/a.txt (cwd is build/tmp), prints progress on stderr and exits with `code`.
static std::string hash_of(const fs::path& dir) {
    std::string e;
    return tree_hash(dir, e);
}

#ifndef _WIN32   // the fake recompiler is a /bin/sh script; the Windows one is run for real by the packaged build
static fs::path fake_recomp(int code) {
    const fs::path p = kBase / ("recomp" + std::to_string(code) + ".sh");
    write(p, "#!/bin/sh\nmkdir -p gen && printf abc > gen/a.txt\necho '[progress] 1/4'\nsleep 0.3\necho '[progress] 3/4'\nsleep 0.3\nexit " +
                 std::to_string(code) + "\n");
    fs::permissions(p, fs::perms::owner_all);
    return p;
}
#endif

int main() {
    fs::remove_all(kBase);
    fs::create_directories(kBase);

    unsigned long d = 0, t = 0;
    CHECK(parse_progress_line("[progress] 3000/14000", d, t) && d == 3000 && t == 14000);
    CHECK(!parse_progress_line("[progress] x/1", d, t) && !parse_progress_line("done 1/2", d, t));
    CHECK(!parse_progress_line("[progress] 1/2 extra", d, t));
    CHECK(parse_module_reference("# c\n[other]\nmodule_abi_sha256 = \"no\"\n[generated]\nmodule_abi_sha256 = \"ab12\"\n") ==
          "ab12");
    CHECK(parse_module_reference("[generated]\n").empty());

    // 1. A file that is not a disc is refused with an existing error id.
    const fs::path junk = kBase / "junk.iso";
    write(junk, "not a disc");
    FrontOutput out;
    StageResult r = identify_stage(junk, nullptr, out);
    CHECK(!r.ok && r.error != ErrorId::None);
    CHECK(!identify_stage(kBase / "missing.iso", nullptr, out).ok);

    // 4 and 5. The recompiler's exit code is mapped, the tree is compared with the reference.
    FrontPaths paths;
    paths.root = kBase / "user";
    paths.install_root = kBase / "inst";
    FrontOutput f;
    f.gen_dir = front_gen_dir(paths.root);
    f.config_file = f.gen_dir.parent_path() / "recomp.toml";
    fs::create_directories(f.gen_dir.parent_path());
    write(f.config_file, "");

#ifndef _WIN32   // stages 4 and 5 run the shell-script recompiler above (see fake_recomp)
    paths.recomp = fake_recomp(0);
    double last = -1;
    std::vector<double> seen;  // the fake prints its progress on stdout, like ps2_recomp
    r = recompile_stage(paths, 10000, [&](double fr, const std::string&) { last = fr; seen.push_back(fr); }, f);
    CHECK(r.ok && f.recomp_exit == 0 && last == 1.0);
    CHECK(std::find(seen.begin(), seen.end(), 0.25) != seen.end() && std::find(seen.begin(), seen.end(), 0.75) != seen.end());

    // "abc" under a.txt: the reference of the tree the fake wrote, computed by the same function.
    std::string err;
    const std::string good = hash_of(f.gen_dir);
    write(paths.install_root / "config" / "module_reference.toml", "[generated]\nmodule_abi_sha256 = \"" + good + "\"\n");
    CHECK(verify_stage(paths, f).ok && f.tree_hash == good);
    write(paths.install_root / "config" / "module_reference.toml",
          "[generated]\nmodule_abi_sha256 = \"" + std::string(64, '0') + "\"\n");
    r = verify_stage(paths, f);
    CHECK(!r.ok && r.error == ErrorId::E52);
    fs::remove(paths.install_root / "config" / "module_reference.toml");
    CHECK(verify_stage(paths, f).error == ErrorId::E50);

    for (int code : {2, 3, 4}) {
        paths.recomp = fake_recomp(code);
        FrontOutput g = f;
        r = recompile_stage(paths, 10000, nullptr, g);
        CHECK(!r.ok && r.error == ErrorId::E51 && g.recomp_exit == code);
    }
    paths.recomp = kBase / "nothing-here";
    CHECK(recompile_stage(paths, 1000, nullptr, f).error == ErrorId::E50 ||
          recompile_stage(paths, 1000, nullptr, f).error == ErrorId::E51);
    paths.recomp = fake_recomp(0);
    r = recompile_stage(paths, 50, nullptr, f);  // killed by the time limit
    CHECK(!r.ok && r.error == ErrorId::E51);

#else
    std::cerr << "module_front: recompile/verify stages skipped, the fake recompiler is a shell script\n";
#endif

    // 4b. The IOP module stage (T-694): a synthetic IRX, packed in 1000.FPK or loose; absent; damaged.
    {
        FrontPaths ip;
        ip.root = kBase / "iop-user";
        FrontOutput o;
        o.game_dir = kBase / "iop-game";
        o.gen_dir = front_gen_dir(ip.root);
        fs::remove_all(o.game_dir);
        fs::remove_all(ip.root);
        const fs::path file = o.gen_dir / "iop_native" / "adxi_native.cpp";

        CHECK(iop_native_stage(ip, o).ok && o.iop_native_note.find("not found") != std::string::npos);  // no MODULES
        CHECK(!fs::exists(file));

        write(o.game_dir / "MODULES" / "CRI_ADXI.IRX", synthetic_irx());  // loose
        StageResult s = iop_native_stage(ip, o);
        CHECK(s.ok && fs::exists(file) && o.iop_native_note.find("entry points") != std::string::npos);
        std::string loose;
        { std::ifstream in(file, std::ios::binary); loose.assign(std::istreambuf_iterator<char>(in), {}); }
        CHECK(loose.rfind("// Generated by tools/iop_recomp/emit_adxi.py", 0) == 0 && loose.find("kCodeLo = 0x6e000u;") != std::string::npos);

        fs::remove(o.game_dir / "MODULES" / "CRI_ADXI.IRX");  // packed (lower-case folder name too)
        fs::remove_all(o.game_dir / "MODULES");
        write(o.game_dir / "modules" / "1000.fpk", synthetic_pack(synthetic_irx()));
        fs::remove(file);
        CHECK(iop_native_stage(ip, o).ok && fs::exists(file));
        std::string packed;
        { std::ifstream in(file, std::ios::binary); packed.assign(std::istreambuf_iterator<char>(in), {}); }
        CHECK(packed == loose);  // the same module gives the same text wherever it comes from

        write(o.game_dir / "modules" / "1000.fpk", synthetic_pack("MZ"));  // a damaged module is E51
        s = iop_native_stage(ip, o);
        CHECK(!s.ok && s.error == ErrorId::E51);
    }

    std::cerr << (g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
