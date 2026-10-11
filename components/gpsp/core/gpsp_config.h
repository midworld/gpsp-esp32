
#ifndef GPSP_CONFIG_H
#define GPSP_CONFIG_H

#define GPSP_NAME                "gpSP"
#define GPSP_VERSION             "v1.1.0"
#define GPSP_NETPACKET_VERSION   "gpSP v1.0"

/* Default ROM buffer size in megabytes (this is a maximum value!) */
#ifndef ROM_BUFFER_SIZE
#define ROM_BUFFER_SIZE 32
#endif

/* Cache sizes and their config knobs */
#define GPSP_RV32_BRANCH_CACHE_SIZE 1024
#ifndef GPSP_RV32_CODE_CHUNK_KIB
#define GPSP_RV32_CODE_CHUNK_KIB 256
#endif
#if defined(GPSP_RV32_JIT)
  #ifndef GPSP_RV32_ROM_CACHE_KIB
    #define GPSP_RV32_ROM_CACHE_KIB 2048
  #endif
  #ifndef GPSP_RV32_RAM_CACHE_KIB
    #define GPSP_RV32_RAM_CACHE_KIB 2048
  #endif
  /* Each cache starts with one chunk and grows up to its limit. */
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * GPSP_RV32_ROM_CACHE_KIB)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * GPSP_RV32_RAM_CACHE_KIB)
#elif defined(SMALL_TRANSLATION_CACHE)
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 2)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 384)
#else
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 10)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 512)
#endif

/* Should be an upperbound to the maximum number of bytes a single JIT'ed
   instruction can take. STM/LDM are tipically the biggest ones */
#define TRANSLATION_CACHE_LIMIT_THRESHOLD (1024 * 2)

/* Hash table size for ROM trans cache lookups */
#if defined(GPSP_RV32_JIT)
#define ROM_BRANCH_HASH_BITS                           12
#else
#define ROM_BRANCH_HASH_BITS                           16
#endif
#define ROM_BRANCH_HASH_SIZE   (1 << ROM_BRANCH_HASH_BITS)

/* RFU Multiplayer config, do not mess around too much with it */
#define MAX_RFU_NETPLAYERS       32

/* Serial modes (multiplayer serial). */
#define MAX_SERMULT_NETPLAYERS    4

#endif
