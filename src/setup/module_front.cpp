#include "module_front.h"
#include "path_text.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "disc_manifest.h"
#include "fpk.h"
#include "iop_emit.h"
#include "platform/process.h"
#include "recomp_config.h"
#include "tree_hash.h"

namespace don_setup {

namespace fs = std::filesystem;

namespace {

constexpr const char* kProgramFile = "SLPS_256.75";  // the game's program on the disc
constexpr const char* kModulePack = "1000.FPK";       // holds the IOP modules, cri_adxi's among them
constexpr const char* kLooseAdxi = "CRI_ADXI.IRX";    // the same module, when not packed
const std::string kAdxiName = "cri_adxi.irx";         // lower case: pack entry names end with it
constexpr int kPollMs = 100;
constexpr size_t kStdoutKeep = 64 * 1024;
// Exit codes of ps2_recomp (ps2xRecomp/include/ps2recomp/recompiler_exit.h, patch 0089).
constexpr int kExitOk = 0;

StageResult fail(ErrorId id, const std::string& why) {
    StageResult r;
    r.ok = false;
    r.error = id;
    r.detail = why;
    return r;
}

bool read_text(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

fs::path find_nocase(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (lower(path_text(e.path().filename())) == lower(name)) return e.path();
    return {};
}

}  // namespace

fs::path front_gen_dir(const fs::path& root) { return root / "build" / "tmp" / "gen"; }

bool parse_progress_line(const std::string& line, unsigned long& done, unsigned long& total) {
    static const std::string tag = "[progress] ";
    if (line.compare(0, tag.size(), tag) != 0) return false;
    size_t i = tag.size(), n = line.size();
    auto number = [&](unsigned long& v) {
        size_t start = i;
        v = 0;
        while (i < n && std::isdigit(static_cast<unsigned char>(line[i]))) v = v * 10 + unsigned(line[i++] - '0');
        return i > start;
    };
    if (!number(done) || i >= n || line[i++] != '/' || !number(total)) return false;
    return i == n || line[i] == '\r';
}

std::string parse_module_reference(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    bool in_generated = false;
    while (std::getline(in, line)) {
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == '#') continue;
        line = line.substr(a);
        if (line[0] == '[') {
            in_generated = line.rfind("[generated]", 0) == 0;
            continue;
        }
        if (!in_generated || line.rfind("module_abi_sha256", 0) != 0) continue;
        size_t q1 = line.find('"'), q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return line.substr(q1 + 1, q2 - q1 - 1);
    }
    return {};
}

StageResult identify_stage(const fs::path& image, const ProgressFn& progress, FrontOutput& out) {
    out.identity = identify_disc(image, progress);
    if (!out.identity.known)
        return fail(out.identity.error == ErrorId::None ? ErrorId::E03 : out.identity.error, out.identity.detail);
    return {};
}

StageResult extract_stage(const fs::path& image, const FrontPaths& paths, const ProgressFn& progress,
                          FrontOutput& out) {
    std::string text, err;
    if (!read_text(paths.install_root / "config" / "disc_manifest.toml", text))
        return fail(ErrorId::E50, "config/disc_manifest.toml is missing");
    std::vector<DiscManifest> manifests;
    if (!parse_disc_manifest(text, manifests, err)) return fail(ErrorId::E50, err);
    const auto m = std::find_if(manifests.begin(), manifests.end(),
                                [&](const DiscManifest& d) { return d.variant_id == out.identity.variant_id; });
    if (m == manifests.end()) return fail(ErrorId::E50, "no allow-list for the variant " + out.identity.variant_id);
    out.game_dir = paths.root / "game" / out.identity.variant_id;
    ErrorId code = ErrorId::None;
    std::string detail;
    if (!extract_disc(image, out.game_dir, progress, err, *m, &code, &detail))
        return fail(code == ErrorId::None ? ErrorId::E31 : code, err + (detail.empty() ? "" : " (" + detail + ")"));
    return {};
}

StageResult config_stage(const FrontPaths& paths, FrontOutput& out) {
    std::string frozen, rendered, err;
    if (!read_text(paths.install_root / "config" / "recomp.toml", frozen))
        return fail(ErrorId::E50, "config/recomp.toml is missing");
    const fs::path disc = find_nocase(out.game_dir, kProgramFile);
    if (disc.empty()) return fail(ErrorId::E77, std::string(kProgramFile) + " is not in the extracted game files");
    out.gen_dir = front_gen_dir(paths.root);
    if (!render_recomp_config(frozen, disc, out.gen_dir, paths.install_root, true, rendered, err))
        return fail(ErrorId::E50, err);
    out.config_file = out.gen_dir.parent_path() / "recomp.toml";
    std::error_code ec;
    fs::remove_all(out.gen_dir, ec);
    fs::create_directories(out.gen_dir.parent_path(), ec);
    std::ofstream f(out.config_file, std::ios::binary | std::ios::trunc);
    f << rendered;
    f.close();
    if (!f) return fail(ErrorId::E31, "cannot write " + path_text(out.config_file));
    return {};
}

StageResult recompile_stage(const FrontPaths& paths, long timeout_ms, const ProgressFn& progress, FrontOutput& out) {
    SpawnOptions o;
    o.argv = {path_text(paths.recomp), path_text(out.config_file)};
    o.env = {"LC_ALL=C"};
    o.cwd = path_text(out.gen_dir.parent_path());
    o.timeout_ms = timeout_ms;
    o.stdout_limit = kStdoutKeep;  // the recompiler prints "[progress] d/t" on stdout
    Process p;
    std::string err;
    if (!spawn(o, p, err)) return fail(ErrorId::E50, "cannot start the recompiler: " + err);
    ExitStatus st;
    unsigned long last = 0;
    while (!poll(p, st)) {
        // The newest "[progress] d/t" line of the captured stdout tail.
        std::istringstream in(p.stdout_tail);
        std::string line;
        unsigned long d = 0, t = 0, best_d = 0, best_t = 0;
        while (std::getline(in, line))
            if (parse_progress_line(line, d, t)) { best_d = d; best_t = t; }
        if (best_t != 0 && best_d != last) {
            last = best_d;
            if (progress) progress(double(best_d) / double(best_t), "translating the game's code");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
    out.recomp_exit = st.exited ? st.code : -1;
    const std::string tail = p.stderr_tail.size() > 400 ? p.stderr_tail.substr(p.stderr_tail.size() - 400) : p.stderr_tail;
    if (st.timed_out) return fail(ErrorId::E51, "the recompiler ran out of time");
    if (!st.exited) return fail(ErrorId::E51, "the recompiler stopped with signal " + std::to_string(st.signal));
    if (st.code != kExitOk) return fail(ErrorId::E51, "the recompiler exited with " + std::to_string(st.code) + ": " + tail);
    if (progress) progress(1.0, "translating the game's code");
    return {};
}

bool find_adxi_irx(const fs::path& game_dir, std::vector<uint8_t>& irx, bool& found) {
    found = false;
    const fs::path modules = find_nocase(game_dir, "MODULES");
    if (modules.empty()) return true;
    std::string pack_text;
    const fs::path pack = find_nocase(modules, kModulePack);
    if (!pack.empty() && read_text(pack, pack_text)) {
        const std::vector<uint8_t> bytes(pack_text.begin(), pack_text.end());
        FpkPack parsed;
        if (fpk_parse(bytes, parsed))
            for (const FpkEntry& e : parsed.entries)
                if (lower(e.name).size() >= kAdxiName.size() &&
                    lower(e.name).compare(lower(e.name).size() - kAdxiName.size(), kAdxiName.size(), kAdxiName) == 0) {
                    found = fpk_read_entry(bytes, e, irx);
                    return found;
                }
    }
    std::string loose_text;
    const fs::path loose = find_nocase(modules, kLooseAdxi);
    if (loose.empty() || !read_text(loose, loose_text)) return true;
    irx.assign(loose_text.begin(), loose_text.end());
    found = true;
    return true;
}

StageResult iop_native_stage(const FrontPaths& paths, FrontOutput& out) {
    (void)paths;
    std::vector<uint8_t> irx;
    bool found = false;
    if (!find_adxi_irx(out.game_dir, irx, found))
        return fail(ErrorId::E51, "the cri_adxi entry of MODULES/1000.FPK cannot be unpacked");
    if (!found) {
        out.iop_native_note = "cri_adxi not found under MODULES: nothing generated, the build goes on without it";
        return {};
    }
    std::string text;
    IopEmitStats st;
    try {
        text = emit_adxi_native(relocate_irx(irx, kAdxiBase), kAdxiBase, kAdxiBase, kAdxiCodeHi, &st);
    } catch (const std::exception& e) {
        return fail(ErrorId::E51, std::string("cri_adxi could not be translated: ") + e.what());
    }
    const fs::path file = out.gen_dir / "iop_native" / "adxi_native.cpp";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f << text;
    f.close();
    if (!f) return fail(ErrorId::E31, "cannot write " + path_text(file));
    out.iop_native_note = std::to_string(st.functions) + " functions (refused " + std::to_string(st.refused) + "), " +
                          std::to_string(st.blocks) + " blocks, " + std::to_string(st.translated_instructions) +
                          " instructions, " + std::to_string(st.entries) + " entry points";
    return {};
}

StageResult verify_stage(const FrontPaths& paths, FrontOutput& out) {
    std::string text, err;
    if (!read_text(paths.install_root / "config" / "module_reference.toml", text))
        return fail(ErrorId::E50, "config/module_reference.toml is missing");
    const std::string want = parse_module_reference(text);
    if (want.empty()) return fail(ErrorId::E50, "config/module_reference.toml has no module_abi_sha256");
    out.tree_hash = tree_hash(out.gen_dir, err);
    if (out.tree_hash.empty()) return fail(ErrorId::E52, err);
    if (out.tree_hash != want) return fail(ErrorId::E52, "generated tree " + out.tree_hash + ", expected " + want);
    return {};
}

StageResult run_front(const fs::path& image, const FrontPaths& paths, long recomp_timeout_ms,
                      const ProgressFn& progress, FrontOutput& out) {
    StageResult r = identify_stage(image, progress, out);
    if (r.ok) r = extract_stage(image, paths, progress, out);
    if (r.ok) r = config_stage(paths, out);
    if (r.ok) r = recompile_stage(paths, recomp_timeout_ms, progress, out);
    if (r.ok) r = iop_native_stage(paths, out);
    if (r.ok) r = verify_stage(paths, out);
    return r;
}

}  // namespace don_setup
