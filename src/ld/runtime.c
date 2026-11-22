#include "ld.h"

const char mcc_executable_crt[] = "<mcc executable startup>";
const char mcc_shared_crt[] = "<mcc shared-library startup>";
const char mcc_runtime_archive[] = "<libmccrt.a>";

bool ld_runtime(Context *c, uint16_t machine, const char *path, Bytes *file)
{
    if (path != mcc_executable_crt && path != mcc_shared_crt && path != mcc_runtime_archive)
        return false;
#ifdef MCC_RUNTIME_SEED
    (void)machine;
    (void)file;
    error(c, (Loc){0}, "the bootstrap compiler can only compile objects");
#else
#define BLOB(name) extern unsigned char name[]; extern const size_t name ## _size
    BLOB(mcc_aarch64_exe);
    BLOB(mcc_aarch64_shared);
    BLOB(mcc_aarch64_archive);
    BLOB(mcc_x86_64_exe);
    BLOB(mcc_x86_64_shared);
    BLOB(mcc_x86_64_archive);
#undef BLOB
#define BYTES(name) ((Bytes){.data = name, .len = name ## _size})
    if (machine == EM_AARCH64) {
        *file = path == mcc_executable_crt ? BYTES(mcc_aarch64_exe) :
                path == mcc_shared_crt ? BYTES(mcc_aarch64_shared) : BYTES(mcc_aarch64_archive);
    } else {
        *file = path == mcc_executable_crt ? BYTES(mcc_x86_64_exe) :
                path == mcc_shared_crt ? BYTES(mcc_x86_64_shared) : BYTES(mcc_x86_64_archive);
    }
#undef BYTES
    (void)c;
#endif
    return true;
}
