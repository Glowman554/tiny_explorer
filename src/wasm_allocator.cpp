/*
 * High-Performance WASM Hybrid Arena Allocator
 * ===========================================
 * 
 * DESIGN GOALS:
 * 1. O(1) allocation/deallocation for millions of small layout objects (GDSTK).
 * 2. Zero memory leak during incremental array growth (OASIS parsing).
 * 3. 16-byte alignment guaranteed for WASM SIMD/Intrinsic compatibility.
 * 
 * ARCHITECTURE:
 * The allocator uses a linked list of large "Arena Blocks" (64MB).
 * Inside these blocks, it uses a hybrid bucketing strategy to minimize fragmentation
 * while allowing instant memory reclamation via free-lists.
 * 
 *   [ Arena Block 0 ] -> [ Arena Block 1 ] -> [ Arena Block N ]
 *          |                    |                    |
 *          v                    v                    v
 *   [H|Slot][H|Slot]...  [H|Slot][H|Slot]...  [H|Slot][BumpPtr...]
 *    ^  ^
 *    |  +-- Data Payload (Rounded to Bucket Size)
 *    +----- 16-byte AllocHeader (Stores logical size)
 * 
 * HYBRID BUCKETING SYSTEM:
 * To balance density (speed) and reuse (memory), we use two tiers of buckets:
 * 
 * 1. GRANULAR TIER (Buckets 0-31):
 *    - Sizes: 16B, 32B, 48B ... up to 512B (16-byte steps).
 *    - Covers 99% of layout objects (Points, Polygons, Nodes).
 *    - Eliminates the "Power of Two Tax" (e.g., a 17B object only takes 32B, not 64B).
 * 
 * 2. EXPONENTIAL TIER (Buckets 32-63):
 *    - Sizes: 1KB, 2KB, 4KB ... up to 4GB+.
 *    - Used for large buffers and growing arrays.
 * 
 * MEMORY RECLAMATION (The "OASIS Fix"):
 * - wasm_free(): Instead of a no-op, it pushes the slot into a per-size bucket.
 * - wasm_realloc(): If a block can't grow in-place, we move it and EXPLICITLY free 
 *   the old slot back into the buckets for immediate reuse.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
    // Global state
    static bool g_use_arena = false;
    static const int chunk_size = 64<<20;
    
    struct ArenaBlock {
        uint8_t* data;
        size_t size;
        size_t offset;
        ArenaBlock* next;
    };

    static ArenaBlock* g_first_block = nullptr;
    static ArenaBlock* g_current_block = nullptr;

    struct FreeNode {
        FreeNode* next;
    };
    
    // Hybrid Buckets: 
    // 0-31: 16-byte granules (0 to 512 bytes)
    // 32-63: Power-of-two (512 bytes to 4GB+)
    static FreeNode* g_free_lists[64];

    struct alignas(16) AllocHeader {
        size_t size;
    };

    static inline int get_bucket_index(size_t size, size_t* out_slot_size) {
        if (size <= 512) {
            size_t s = (size + 15) & ~15;
            if (s == 0) s = 16;
            *out_slot_size = s;
            return (int)(s >> 4) - 1; // 0 to 31
        }
        // Power of two for large objects
#if defined(__LP64__) || defined(__wasm64__)
        int b = 64 - __builtin_clzll((unsigned long long)size - 1);
        *out_slot_size = (size_t)1 << b;
        return 32 + b;
#else
        int b = 32 - __builtin_clz((unsigned int)size - 1);
        *out_slot_size = (size_t)1 << b;
        return 32 + b;
#endif
    }

    void wasm_arena_init(uint32_t mode) {
        g_use_arena = (mode != 0);
        
        for (int i = 0; i < 64; i++) g_free_lists[i] = nullptr;

        ArenaBlock* b = g_first_block;
        while (b) {
            b->offset = 0;
            b = b->next;
        }
        g_current_block = g_first_block;

        if (g_use_arena && !g_first_block) {
            size_t initial_size = chunk_size;
            uint8_t* data = (uint8_t*)malloc(initial_size);
            if (data) {
                g_first_block = (ArenaBlock*)malloc(sizeof(ArenaBlock));
                g_first_block->data = data;
                g_first_block->size = initial_size;
                g_first_block->offset = 0;
                g_first_block->next = nullptr;
                g_current_block = g_first_block;
            }
        }
    }

    uint64_t wasm_arena_get_usage() {
        uint64_t total = 0;
        ArenaBlock* b = g_first_block;
        while (b) {
            total += b->size;
            b = b->next;
        }
        return total;
    }

    static void* arena_alloc(size_t size) {
        size_t slot_size;
        int bucket = get_bucket_index(size, &slot_size);

        // 1. Try free list
        if (bucket < 64 && g_free_lists[bucket]) {
            FreeNode* node = g_free_lists[bucket];
            g_free_lists[bucket] = node->next;
            AllocHeader* header = ((AllocHeader*)node) - 1;
            header->size = size;
            return (void*)node;
        }

        // 2. Bump alloc
        size_t total_size = sizeof(AllocHeader) + slot_size; // slot_size is already 16-aligned

        if (!g_current_block) return nullptr;

        if (g_current_block->offset + total_size > g_current_block->size) {
            if (g_current_block->next) {
                g_current_block = g_current_block->next;
                g_current_block->offset = 0;
            } else {
                size_t next_size = (total_size > (size_t)chunk_size) ? (total_size + 1024) : (size_t)chunk_size;
                uint8_t* data = (uint8_t*)malloc(next_size);
                if (!data) return nullptr;

                ArenaBlock* next_block = (ArenaBlock*)malloc(sizeof(ArenaBlock));
                next_block->data = data;
                next_block->size = next_size;
                next_block->offset = 0;
                next_block->next = nullptr;

                g_current_block->next = next_block;
                g_current_block = next_block;
            }
        }

        AllocHeader* header = (AllocHeader*)(g_current_block->data + g_current_block->offset);
        header->size = size;
        
        void* ptr = (void*)(header + 1);
        g_current_block->offset += total_size;
        return ptr;
    }

    void* wasm_malloc(size_t size) {
        if (g_use_arena) return arena_alloc(size);
        return malloc(size);
    }

    void wasm_free(void* ptr) {
        if (!ptr || !g_use_arena) {
            if (ptr && !g_use_arena) free(ptr);
            return;
        }

        AllocHeader* header = ((AllocHeader*)ptr) - 1;
        size_t dummy;
        int bucket = get_bucket_index(header->size, &dummy);
        if (bucket < 64) {
            FreeNode* node = (FreeNode*)ptr;
            node->next = g_free_lists[bucket];
            g_free_lists[bucket] = node;
        }
    }

    void* wasm_realloc(void* ptr, size_t size) {
        if (!g_use_arena) return realloc(ptr, size);
        if (!ptr) return arena_alloc(size);
        
        AllocHeader* header = ((AllocHeader*)ptr) - 1;
        
        size_t current_slot_size;
        int current_bucket = get_bucket_index(header->size, &current_slot_size);
        
        size_t new_slot_size;
        int new_bucket = get_bucket_index(size, &new_slot_size);

        // If it fits in same bucket, just update logical size
        if (new_bucket <= current_bucket) {
            header->size = size;
            return ptr;
        }

        // Try in-place growth if last
        size_t current_total_size = sizeof(AllocHeader) + current_slot_size;
        if ((uint8_t*)ptr + (current_total_size - sizeof(AllocHeader)) == g_current_block->data + g_current_block->offset) {
            size_t new_total_size = sizeof(AllocHeader) + new_slot_size;
            size_t diff = new_total_size - current_total_size;
            if (g_current_block->offset + diff <= g_current_block->size) {
                g_current_block->offset += diff;
                header->size = size;
                return ptr;
            }
        }

        void* new_ptr = arena_alloc(size);
        if (new_ptr) {
            memcpy(new_ptr, ptr, header->size);
            wasm_free(ptr);
        }
        return new_ptr;
    }

    void* wasm_calloc(size_t nmemb, size_t size) {
        size_t total = nmemb * size;
        void* ptr = wasm_malloc(total);
        if (ptr) memset(ptr, 0, total);
        return ptr;
    }
}

namespace gdstk {
    void* allocate(uint64_t size) { return wasm_malloc((size_t)size); }
    void* reallocate(void* ptr, uint64_t size) { return wasm_realloc(ptr, (size_t)size); }
    void* allocate_clear(uint64_t size) { return wasm_calloc(1, (size_t)size); }
    void free_allocation(void* ptr) { wasm_free(ptr); }
}
