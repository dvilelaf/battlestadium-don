#include "user_paths.h"

#include "../host/user_dir.h"

namespace don_setup {

UserRoots user_roots(ErrorId* err) {
    if (err) *err = ErrorId::None;
    // don_user_dir() falls back to a relative path when HOME is unset; that is never a place to write.
    std::filesystem::path dir = don_host::don_user_dir();
    if (dir.empty() || !dir.is_absolute()) {
        if (err) *err = ErrorId::E32;
        return {};
    }
    return {dir, dir};
}


}  // namespace don_setup
