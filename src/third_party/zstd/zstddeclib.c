/* Omni Sampler 2.4.32: zstd 1.5.5 DECOMPRESSION only (BSD licence, LICENSE beside this file), one translation unit.
   Serum 2 presets (.SerumPreset) hold a zstd frame. No assembly (ZSTD_DISABLE_ASM), no threads, no legacy formats,
   no dictionary builder: the MPC builds (armv7 gnu11, aarch64) compile this file and nothing else from zstd. */
#define ZSTD_DISABLE_ASM 1
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_MULTITHREAD_SUPPORT_DEFAULT 0
#define XXH_NAMESPACE ZSTD_
#define DEBUGLEVEL 0
#include "common/debug.c"
#include "common/entropy_common.c"
#include "common/error_private.c"
#include "common/fse_decompress.c"
#include "common/xxhash.c"
#include "common/zstd_common.c"
#include "decompress/huf_decompress.c"
#include "decompress/zstd_ddict.c"
#include "decompress/zstd_decompress.c"
#include "decompress/zstd_decompress_block.c"
