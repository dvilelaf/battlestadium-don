#include "errors.h"

namespace don_setup {

const ErrorInfo& error_info(ErrorId id) {
    std::size_t n = 0;
    const ErrorEntry* t = error_entries(&n);
    const ErrorInfo* fallback = &t[0].info;
    for (std::size_t i = 0; i < n; ++i) {
        if (t[i].id == id) return t[i].info;
        if (t[i].id == ErrorId::E15) fallback = &t[i].info;
    }
    return *fallback;
}

}  // namespace don_setup
