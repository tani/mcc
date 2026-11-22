#include "ld.h"

#include <string.h>

static PatchResult patch_value(unsigned char *out, size_t len, uint64_t value, unsigned bytes)
{
    if (len < bytes)
        return PATCH_TRUNCATED;
    memcpy(out, &value, bytes);
    return PATCH_OK;
}

static PatchResult patch_x86_64(unsigned char *out, size_t len, uint32_t type,
                               uint64_t value, uint64_t place)
{
    switch (type) {
    case R_X86_64_NONE: return PATCH_OK;
    case R_X86_64_64: return patch_value(out, len, value, 8);
    case R_X86_64_PC64: return patch_value(out, len, value - place, 8);
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
    case R_X86_64_GOTPCREL:
    case R_X86_64_GOTPCRELX:
    case R_X86_64_REX_GOTPCRELX:
    case R_X86_64_TLSGD:
    case R_X86_64_GOTTPOFF:
    case R_X86_64_GOTPC32:
        value -= place;
        /* Fall through. */
    case R_X86_64_32S:
    case R_X86_64_TPOFF32:
    case R_X86_64_DTPOFF32:
        return fits_signed((int64_t)value, 32) ? patch_value(out, len, value, 4) : PATCH_RANGE;
    case R_X86_64_32:
        return value <= UINT32_MAX ? patch_value(out, len, value, 4) : PATCH_RANGE;
    case R_X86_64_GOTPCREL64: case R_X86_64_GOTPC64:
        return patch_value(out, len, value - place, 8);
    case R_X86_64_GOTOFF64: case R_X86_64_GOT64: case R_X86_64_PLTOFF64:
    case R_X86_64_TPOFF64: case R_X86_64_DTPOFF64:
        return patch_value(out, len, value, 8);
    default:
        return PATCH_UNSUPPORTED;
    }
}

static PatchResult patch_aarch64(unsigned char *out, size_t len, uint32_t type,
                                uint64_t value, uint64_t place)
{
    int64_t delta = (int64_t)(value - place);
    unsigned bits = 0, shift = 0, scale = 0;
    uint32_t mask = 0, immediate_bits = 0;
    switch (type) {
    case R_AARCH64_NONE: return PATCH_OK;
    case R_AARCH64_TLSDESC_CALL: case R_AARCH64_TLSDESC_ADD: case R_AARCH64_TLSDESC_LDR:
        return PATCH_OK;
    case R_AARCH64_ABS64: return patch_value(out, len, value, 8);
    case R_AARCH64_ABS32:
        return value <= UINT32_MAX || fits_signed((int64_t)value, 32) ?
               patch_value(out, len, value, 4) : PATCH_RANGE;
    case R_AARCH64_PREL64: return patch_value(out, len, value - place, 8);
    case R_AARCH64_PREL32:
        return fits_signed(delta, 32) ? patch_value(out, len, (uint64_t)delta, 4) : PATCH_RANGE;
    case R_AARCH64_CALL26: case R_AARCH64_JUMP26:
        bits = 26; scale = 2; break;
    case R_AARCH64_CONDBR19: case R_AARCH64_LD_PREL_LO19: case R_AARCH64_GOT_LD_PREL19:
    case R_AARCH64_TLSDESC_LD_PREL19: case R_AARCH64_TLSIE_LD_GOTTPREL_PREL19:
        bits = 19; shift = 5; scale = 2; break;
    case R_AARCH64_TSTBR14:
        bits = 14; shift = 5; scale = 2; break;
    case R_AARCH64_ADR_PREL_LO21:
    case R_AARCH64_TLSDESC_ADR_PREL21:
    case R_AARCH64_ADR_PREL_PG_HI21:
    case R_AARCH64_ADR_PREL_PG_HI21_NC:
    case R_AARCH64_TLSDESC_ADR_PAGE21:
    case R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21:
    case R_AARCH64_ADR_GOT_PAGE: {
        if (type != R_AARCH64_ADR_PREL_LO21 && type != R_AARCH64_TLSDESC_ADR_PREL21)
            delta = (int64_t)((value >> 12) - (place >> 12));
        if (type != R_AARCH64_ADR_PREL_PG_HI21_NC && !fits_signed(delta, 21))
            return PATCH_RANGE;
        uint32_t encoded = (uint32_t)delta & 0x1fffff;
        mask = 0x60ffffe0;
        immediate_bits = ((encoded & 3) << 29) | ((encoded >> 2) << 5);
        break;
    }
    case R_AARCH64_LDST128_ABS_LO12_NC: case R_AARCH64_TLSLE_LDST128_TPREL_LO12_NC: scale = 4; break;
    case R_AARCH64_LDST64_ABS_LO12_NC: case R_AARCH64_LD64_GOT_LO12_NC: case R_AARCH64_TLSDESC_LD64_LO12:
    case R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC: case R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC: scale = 3; break;
    case R_AARCH64_LDST32_ABS_LO12_NC: case R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC: scale = 2; break;
    case R_AARCH64_LDST16_ABS_LO12_NC: case R_AARCH64_TLSLE_LDST16_TPREL_LO12_NC: scale = 1; break;
    case R_AARCH64_TLSLE_ADD_TPREL_HI12:
        if (value >= 0x1000000)
            return PATCH_RANGE;
        value >>= 12;
        break;
    case R_AARCH64_TLSLE_ADD_TPREL_LO12:
        if (value >= 0x1000)
            return PATCH_RANGE;
        /* Fall through. */
    case R_AARCH64_TLSLE_ADD_TPREL_LO12_NC: case R_AARCH64_TLSLE_LDST8_TPREL_LO12_NC:
    case R_AARCH64_TLSDESC_ADD_LO12: case R_AARCH64_ADD_ABS_LO12_NC:
    case R_AARCH64_LDST8_ABS_LO12_NC:
        break;
    case R_AARCH64_MOVW_UABS_G0: case R_AARCH64_MOVW_UABS_G0_NC:
    case R_AARCH64_MOVW_UABS_G1: case R_AARCH64_MOVW_UABS_G1_NC:
    case R_AARCH64_MOVW_UABS_G2: case R_AARCH64_MOVW_UABS_G2_NC:
    case R_AARCH64_MOVW_UABS_G3: {
        unsigned group = (type - R_AARCH64_MOVW_UABS_G0) / 2;
        if (!((type - R_AARCH64_MOVW_UABS_G0) & 1) && group < 3 && value >> ((group + 1) * 16))
            return PATCH_RANGE;
        mask = 0x001fffe0;
        immediate_bits = ((uint32_t)(value >> (group * 16)) & 0xffff) << 5;
        break;
    }
    default: return PATCH_UNSUPPORTED;
    }
    if (bits) {
        int64_t divisor = INT64_C(1) << scale;
        if (delta % divisor || !fits_signed(delta / divisor, bits))
            return PATCH_RANGE;
        uint32_t field = (UINT32_C(1) << bits) - 1;
        mask = field << shift;
        immediate_bits = ((uint32_t)(delta / divisor) & field) << shift;
    } else if (!mask) {
        if (value & ((UINT64_C(1) << scale) - 1))
            return PATCH_RANGE;
        mask = 0x003ffc00;
        immediate_bits = ((uint32_t)(value & 0xfff) >> scale) << 10;
    }
    if (len < sizeof(uint32_t))
        return PATCH_TRUNCATED;
    uint32_t instruction;
    memcpy(&instruction, out, sizeof instruction);
    instruction = (instruction & ~mask) | immediate_bits;
    memcpy(out, &instruction, sizeof instruction);
    return PATCH_OK;
}

PatchResult ld_patch(uint16_t machine, unsigned char *out, size_t len, uint32_t type,
                     uint64_t value, uint64_t place)
{
    return machine == EM_AARCH64 ? patch_aarch64(out, len, type, value, place) :
                                  patch_x86_64(out, len, type, value, place);
}
