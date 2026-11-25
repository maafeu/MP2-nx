// enum ZydisMnemonic from ZydisMnemonics.def
enum ZydisMnemonic : uint16_t {
#define X(n) ZYDIS_MNEMONIC_##n,
#include "ZydisMnemonics.def"
#undef X
    ZYDIS_MNEMONIC_MAX_VALUE
};
