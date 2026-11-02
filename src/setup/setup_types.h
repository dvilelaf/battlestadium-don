// Types shared by the first-run setup: the disc verdict, the error catalogue IDs, the disc identity and the
// preparation progress. Header only; no file system, no drawing.
#pragma once

#include <cstdint>
#include <string>

namespace don_setup {

// Why a disc image was accepted or refused. The English sentences live in the screen model.
enum class DiscVerdict { Ok, Unreadable, NotDon, Modified, UnknownData };

// Every ID of the error catalogue (docs/tasks/M6-public-release.md, A4). The number is the ID.
// None (0) is "no error"; the catalogue has no such value, so it is added here.
enum class ErrorId : uint16_t {
    None = 0,
    E01 = 1,
    E02 = 2,
    E03 = 3,
    E04 = 4,
    E05 = 5,
    E06 = 6,
    E07 = 7,
    E08 = 8,
    E09 = 9,
    E10 = 10,
    E11 = 11,
    E12 = 12,
    E13 = 13,
    E14 = 14,
    E15 = 15,
    E16 = 16,
    E17 = 17,
    E18 = 18,
    E19 = 19,
    E20 = 20,
    E21 = 21,
    E22 = 22,
    E30 = 30,
    E31 = 31,
    E32 = 32,
    E33 = 33,
    E34 = 34,
    E35 = 35,
    E36 = 36,
    E37 = 37,
    E38 = 38,
    E39 = 39,
    E40 = 40,
    E41 = 41,
    E42 = 42,
    E50 = 50,
    E51 = 51,
    E52 = 52,
    E53 = 53,
    E54 = 54,
    E55 = 55,
    E56 = 56,
    E57 = 57,
    E58 = 58,
    E59 = 59,
    E70 = 70,
    E71 = 71,
    E72 = 72,
    E73 = 73,
    E74 = 74,
    E75 = 75,
    E76 = 76,
    E77 = 77,
    E78 = 78,
    E79 = 79,
    E80 = 80,
    E81 = 81,
    E82 = 82,
    E83 = 83,
};

// What identify_disc returns. `reason` is Ok when known is true.
struct DiscIdentity {
    bool known = false;
    std::string variant_id;
    std::string description;
    DiscVerdict reason = DiscVerdict::Ok;
    ErrorId error = ErrorId::None;  // the catalogue entry that says what is wrong; None when known is true
    // Facts for the message: the serial for E13 and E16, "have of want" bytes for E08, the percent read for E19,
    // the container name for E10 and E12, a short hint otherwise.
    std::string detail;
};

enum class Stage { Copy, Translate, Build, Verify, Done, Failed };

struct Progress {
    Stage stage = Stage::Copy;
    double fraction = 0.0;  // overall, 0..1
    double seconds_left = 0.0;
    int parts_done = 0;
    int parts_total = 0;
    std::string detail;  // free text; on Failed it is the reason
};

}  // namespace don_setup
