// SPDX-License-Identifier: GPL-3.0-or-later
// The measuring program of the profiler test (tests/profile/): it samples itself, so that the
// sampler and the summary can be checked without the game and without a runtime.
//
// A worker thread calls profile_thread_begin(), spends a calibrated number of iterations of two
// distinct noinline functions in a known proportion of time (about 1.4 s in busy_a and 0.6 s in
// busy_b), then calls profile_thread_end() and profile_finish(). The main thread sleeps while this
// happens: every sample of the profile must be the worker's, never the sleeper's.
//
// It prints the figures tests/profile/test_profile_tests.py checks:
//   work_ms:<float>   wall time of the worker's measured work (the same work with and without the
//                     profiler, so this is where its overhead shows)
//   cpu_ms:<float>    processor time of the whole process over the same work (the sampler's signal
//                     handler is processor time too, and this figure does not move when the
//                     machine preempts the thread, which is what the wall time cannot avoid)
//   wall_ms:<float>   wall time of the whole program (dominated by the main thread's sleep)
//   sink:<float>      the volatile the two functions accumulate, so nothing is optimised away
//   a_share_target:<float>  the proportion of time the busy_a phase was sized to (0.7)
//   busy_a_iterations / busy_b_iterations: the work it did, so a second run can be given the same
//                     work with `profile_probe <a_iterations> <b_iterations>` (that is how the test
//                     compares the run with the profiler against the one without it)
#include "profile.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

#if defined(_MSC_VER)
#define DON_NOINLINE __declspec(noinline)
#else
#define DON_NOINLINE __attribute__((noinline))
#endif

namespace
{
using Clock = std::chrono::steady_clock;

// The result of the two busy functions. It is volatile so the compiler has to keep the arithmetic:
// the inner loops change a value derived from it and store it back here.
volatile double g_sink = 1.0;

// The two measured functions. Different constants and noinline keep them apart: the compiler
// cannot merge them or fold the arithmetic away, and the samples stay inside them.
DON_NOINLINE double busy_a(double seed, uint64_t iterations)
{
    double x = seed * 1.000001;
    for (uint64_t i = 0; i < iterations; ++i)
    {
        x = x * 1.0000001 + 0.25;
        x = x / 1.0000003 - 0.125;
        x = x * 0.9999997 + 0.0625;
    }
    g_sink = x;
    return x;
}

DON_NOINLINE double busy_b(double seed, uint64_t iterations)
{
    double x = seed * 0.999999;
    for (uint64_t i = 0; i < iterations; ++i)
    {
        x = x * 0.9999997 - 0.125;
        x = x / 0.9999999 + 0.0625;
        x = x * 1.0000003 - 0.25;
    }
    g_sink = x;
    return x;
}

constexpr uint64_t kCalibrationIterations = 200000;
constexpr uint64_t kCalibrationRounds = 20;  // about 20 ms of work per trial
constexpr int kCalibrationTrials = 3;        // the smallest of them is used
constexpr double kPhaseASeconds = 1.4;
constexpr double kPhaseBSeconds = 0.6;

double milliseconds(Clock::time_point from, Clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// Nanoseconds per iteration, from warm measurements of a fixed number of iterations. The smallest
// of several trials is the answer: another process on the machine can only make a trial slower,
// never faster, so the minimum is the closest to the cost of the work itself.
double nanoseconds_per_iteration(double (*function)(double, uint64_t), double seed)
{
    function(seed, kCalibrationIterations); // warm up: touch the code and the stack
    double best = 0.0;
    for (int trial = 0; trial < kCalibrationTrials; ++trial)
    {
        const uint64_t iterations = kCalibrationIterations * kCalibrationRounds;
        const auto from = Clock::now();
        function(seed, iterations);
        const auto to = Clock::now();
        const double nanoseconds = std::chrono::duration<double, std::nano>(to - from).count() /
                                   static_cast<double>(iterations);
        if (best == 0.0 || nanoseconds < best)
            best = nanoseconds;
    }
    return best;
}

uint64_t iterations_for(double nanoseconds_per_iteration_, double seconds)
{
    const double count = seconds * 1.0e9 / nanoseconds_per_iteration_;
    return count < 1.0 ? 1 : static_cast<uint64_t>(count);
}

struct Work
{
    uint64_t a_iterations = 0;
    uint64_t b_iterations = 0;
    double work_ms = 0.0;
    double cpu_ms = 0.0;
};

Work g_work;

void worker(const Work &work)
{
    profile_thread_begin("profile-probe");
    const auto from = Clock::now();
    const std::clock_t cpu_from = std::clock();
    double seed = g_sink;
    seed = busy_a(seed, work.a_iterations);
    seed = busy_b(seed, work.b_iterations);
    g_work.cpu_ms = 1000.0 * static_cast<double>(std::clock() - cpu_from) / CLOCKS_PER_SEC;
    g_work.work_ms = milliseconds(from, Clock::now());
    g_sink = seed;
    profile_thread_end();
    profile_finish();
}
} // namespace

int main(int argc, char **argv)
{
    const auto program_from = Clock::now();

    // A previous run can hand over its work (`profile_probe <a_iterations> <b_iterations>`), so two
    // runs do literally the same number of iterations and only the profiler differs between them.
    uint64_t a_iterations = 0;
    uint64_t b_iterations = 0;
    if (argc == 3)
    {
        a_iterations = std::strtoull(argv[1], nullptr, 10);
        b_iterations = std::strtoull(argv[2], nullptr, 10);
    }

    // Size the two phases by measured iteration cost, so the split is a proportion of *time*.
    if (a_iterations == 0 || b_iterations == 0)
    {
        const double a_ns = nanoseconds_per_iteration(&busy_a, g_sink);
        const double b_ns = nanoseconds_per_iteration(&busy_b, g_sink);
        std::printf("calibration_ns_per_iteration: busy_a=%.3f busy_b=%.3f\n", a_ns, b_ns);
        a_iterations = iterations_for(a_ns, kPhaseASeconds);
        b_iterations = iterations_for(b_ns, kPhaseBSeconds);
    }
    std::printf("busy_a_iterations: %llu\n", static_cast<unsigned long long>(a_iterations));
    std::printf("busy_b_iterations: %llu\n", static_cast<unsigned long long>(b_iterations));

    Work work;
    work.a_iterations = a_iterations;
    work.b_iterations = b_iterations;

    // The main thread sleeps through the whole measurement: it must get no samples.
    std::thread worker_thread(worker, work);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    worker_thread.join();

    std::printf("a_share_target: %.3f\n", kPhaseASeconds / (kPhaseASeconds + kPhaseBSeconds));
    std::printf("work_ms: %.1f\n", g_work.work_ms);
    std::printf("cpu_ms: %.1f\n", g_work.cpu_ms);
    std::printf("wall_ms: %.1f\n", milliseconds(program_from, Clock::now()));
    std::printf("sink: %.6f\n", static_cast<double>(g_sink));
    return 0;
}
