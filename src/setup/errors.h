// The error table of the first-run setup: for every ErrorId (docs/tasks/M6-public-release.md, A4) a short code, a
// title, why it happened, what to do and the primary action, so every screen shows an error the same way (S5).
//
// Placeholders the screen fills in before showing a text:
//   {name}    file or folder name           {serial}  disc serial of another game, e.g. SLES-12345
//   {gb}      gigabytes, e.g. "1.7"         {have}, {total}  sizes in GB of an incomplete image
//   {percent} progress at a failure, "61"   {file}    a file inside the image, e.g. BGM.AFS
//   {reason}  the system's error text, e.g. "input/output error"
//
// A "silent" entry (A4 says nothing is shown) has a title for logs and an empty why, what_to_do and action.
#pragma once

#include <cstddef>

#include "setup_types.h"

namespace don_setup {

struct ErrorInfo {
    const char* code;         // unique, "E-SPACE-2" style: an area word and a number
    const char* title;        // at most 40 characters
    const char* why;          // what happened and why; empty when silent
    const char* what_to_do;   // the advice sentence(s), without a prefix; empty when silent
    const char* action;       // primary button, upper case, e.g. "CHOOSE ANOTHER DISC"; empty when silent
};

struct ErrorEntry {
    ErrorId id;
    ErrorInfo info;
};

// The info of `id`. An id that is not in the catalogue gets the entry of E15 (never null, never throws).
const ErrorInfo& error_info(ErrorId id);

// Every entry, in ID order; *count receives their number.
const ErrorEntry* error_entries(std::size_t* count);

}  // namespace don_setup
