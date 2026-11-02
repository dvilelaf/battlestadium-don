// T-718: the time-left text the player reads during the prepare, driven through the real sequence (no SDL).
// Copy, translate and the start of the build have no rate (unknown); the screen must say so until the build has one.
#include "../../src/host/setup_screen_model.cpp"
#include "../../src/host/setup_flow_model.cpp"

#include <cmath>
#include <cstdio>

using namespace don_setup;

static int g_fail = 0, g_checks = 0;
#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        std::string _a = (a), _b = (b);                                                             \
        if (_a != _b) {                                                                             \
            ++g_fail;                                                                               \
            std::printf("FAIL %s:%d  [%s] != [%s]\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); \
        }                                                                                           \
    } while (0)

static const char* const kEstimating = "Estimating time left\xE2\x80\xA6";

static Progress step(PrepStage s, double f, double left) {
    Progress p;
    p.stage = s;
    p.fraction = f;
    p.seconds_left = left;
    return p;
}

static SetupModel preparing() {
    SetupModel m;
    Environment e;
    e.free_bytes = 50ull * 1000000000ull;
    e.needed_bytes = 3200000000ull;
    e.drive = "/home";
    e.cores = 8;
    e.ram_bytes = 16ull << 30;
    e.estimate_seconds = 600;
    m.set_environment(e);
    m.file_chosen("/tmp/don.iso");
    DiscIdentity id;
    id.known = true;
    id.variant_id = "en";
    id.description = "English fan translation";
    m.set_disc_identity(id);
    m.take_actions();
    return m;
}

static std::string about_min(double seconds) {
    return "About " + std::to_string(std::lround(seconds / 60.0)) + " min left";
}

static void test_unknown_until_the_build_has_a_rate() {
    SetupModel m = preparing();
    // The values come from the driver's own rule: the build starts at 0.15 and takes 480 s for the rest.
    const double s0 = 0.15, build_s = 480;
    auto left_at = [&](double f) {
        const double in_stage = (f - s0) / (1.0 - s0) * build_s;
        return stage_seconds_left(f, s0, in_stage, 1.0, kEtaMinStageFraction);
    };
    // Copy and translate: unknown from the driver.
    for (double f : {0.0, 0.05, 0.10, 0.15}) {
        m.set_progress(step(f < 0.05 ? PrepStage::Copy : PrepStage::Translate, f, kSecondsLeftUnknown));
        CHECK_EQ(m.view().time_text, kEstimating);
    }
    // The build begins and runs up to 2 % of its range: still unknown, still not "Less than a minute".
    for (double f : {0.15, 0.155, 0.16, 0.165}) {
        const double got = left_at(f);
        m.set_progress(step(PrepStage::Build, f, got));
        CHECK_EQ(m.view().time_text, kEstimating);
    }
    // Real values: the first is taken as is (no cap from the unknown), then it follows the true time.
    for (double f : {0.20, 0.35, 0.5, 0.8}) {
        const double truth = build_s * (1.0 - f) / (1.0 - s0);
        m.set_progress(step(PrepStage::Build, f, left_at(f)));
        CHECK_EQ(m.view().time_text, truth < 60 ? "Less than a minute left" : about_min(truth));
    }
}

int main() {
    test_unknown_until_the_build_has_a_rate();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
