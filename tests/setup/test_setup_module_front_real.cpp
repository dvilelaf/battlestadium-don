// Real run of stages 1 to 5 (4b, the IOP module, included) of the front half of prepare (T-691). Skips (exit 0) without arguments, so ctest passes
// everywhere. Not a unit test: it reads the player's disc and runs the bundled recompiler.
//   test_setup_module_front_real --iso <image or folder> --root <user folder> --recomp <ps2_recomp> --install-root <dir>
#include <chrono>
#include <iostream>
#include <map>

#include "module_front.h"

using namespace don_setup;

int main(int argc, char** argv) {
    std::map<std::string, std::string> a;
    for (int i = 1; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    if (a.count("--iso") == 0 || a.count("--root") == 0 || a.count("--recomp") == 0 || a.count("--install-root") == 0) {
        std::cout << "skipped: needs --iso --root --recomp --install-root\n";
        return 0;
    }
    FrontPaths paths{a["--root"], a["--install-root"], a["--recomp"]};
    FrontOutput out;
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    auto lap = [&](const char* name, const StageResult& r) {
        const auto now = Clock::now();
        std::cout << "stage " << name << ": " << (r.ok ? "ok" : "FAILED") << " in "
                  << std::chrono::duration<double>(now - t0).count() << " s\n";
        t0 = now;
        if (!r.ok) std::cout << "error E" << int(r.error) << ": " << r.detail << "\n";
        return r.ok;
    };
    int last_pct = -1;
    auto progress = [&](double f, const std::string& what) {
        const int pct = int(f * 10) * 10;
        if (pct != last_pct) { last_pct = pct; std::cout << "  " << what << " " << pct << "%\n"; }
    };
    bool ok = lap("identify", identify_stage(a["--iso"], progress, out));
    if (ok) std::cout << "variant " << out.identity.variant_id << "\n";
    ok = ok && lap("extract", extract_stage(a["--iso"], paths, progress, out));
    ok = ok && lap("config", config_stage(paths, out));
    last_pct = -1;
    ok = ok && lap("recompile", recompile_stage(paths, 60L * 60 * 1000, progress, out));
    std::cout << "recompiler exit " << out.recomp_exit << "\n";
    ok = ok && lap("iop-native", iop_native_stage(paths, out));
    if (ok) std::cout << "iop native: " << out.iop_native_note << "\n";
    const StageResult v = ok ? verify_stage(paths, out) : StageResult{};
    if (ok) lap("verify", v);
    std::cout << "tree hash " << out.tree_hash << "\n";
    std::cout << "exit " << (ok && v.ok ? 0 : 1) << "\n";
    return ok && v.ok ? 0 : 1;
}
