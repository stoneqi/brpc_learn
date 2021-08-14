// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// bthread - A M:N threading library to make applications more concurrent.

// Date: Sun Jul 13 15:04:18 CST 2014

#ifndef BUTIL_RESOURCE_POOL_INL_H
#define BUTIL_RESOURCE_POOL_INL_H

#include <iostream>                      // std::ostream
#include <pthread.h>                     // pthread_mutex_t
#include <algorithm>                     // std::max, std::min
#include "butil/atomicops.h"              // butil::atomic
#include "butil/macros.h"                 // BAIDU_CACHELINE_ALIGNMENT
#include "butil/scoped_lock.h"            // BAIDU_SCOPED_LOCK
#include "butil/thread_local.h"           // thread_atexit
#include <vector>

#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
#define BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_ADD1                \
    (_global_nfree.fetch_add(1, butil::memory_order_relaxed))
#define BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_SUB1                \
    (_global_nfree.fetch_sub(1, butil::memory_order_relaxed))
#else
#define BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_ADD1
#define BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_SUB1
#endif

namespace butil {
    
// 使用 ResourceId 来管理对应的资源
template <typename T>
struct ResourceId {
    uint64_t value;

    operator uint64_t() const {
        return value;
    }

    template <typename T2>
    ResourceId<T2> cast() const {
        ResourceId<T2> id = { value };
        return id;
    }
};


// 保存已释放的 T 的 ID值
template <typename T, size_t NITEM> 
struct ResourcePoolFreeChunk {
    size_t nfree;
    ResourceId<T> ids[NITEM];
};
// for gcc 3.4.5
// 柔性数组，节省内存
template <typename T> 
struct ResourcePoolFreeChunk<T, 0> {
    size_t nfree;
    ResourceId<T> ids[0];
};

// 当前资源池信息统计
struct ResourcePoolInfo {
    size_t local_pool_num;
    size_t block_group_num;
    size_t block_num;
    size_t item_num;
    size_t block_item_num;
    size_t free_chunk_item_num;
    size_t total_size;
#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
    size_t free_item_num;
#endif
};

// BLOCK_NGROUP 最大值
static const size_t RP_MAX_BLOCK_NGROUP = 65536;
// 偏移量，计算GROUP index时偏移16位。 ResourceID 16位前为 GROUP index， 
static const size_t RP_GROUP_NBLOCK_NBIT = 16;
// GROUP_NBLOCK 数目
static const size_t RP_GROUP_NBLOCK = (1UL << RP_GROUP_NBLOCK_NBIT);
// 初始化 FREE_LIST 大小
static const size_t RP_INITIAL_FREE_LIST_SIZE = 1024;

// 该类用于计算一个block能存下几个T
// 如果T大于64*1024 则，存储一个，否则按照 N1计算。 
// 同时，一个block最大存储256个元素
// 注意该类在编译时会直接计算出来，运行时为常量、
template <typename T>
class ResourcePoolBlockItemNum {
    // Block 的内存大小为 64 * 1024; 元素大于1024 则该block只保存一个。
    static const size_t N1 = ResourcePoolBlockMaxSize<T>::value / sizeof(T);
    static const size_t N2 = (N1 < 1 ? 1 : N1);
public:
    // min(ResourcePoolBlockMaxItem<T>::value ,  N2 ) 最大为256个元素
    static const size_t value = (N2 > ResourcePoolBlockMaxItem<T>::value ?
                                 ResourcePoolBlockMaxItem<T>::value : N2);
};


template <typename T>
class BAIDU_CACHELINE_ALIGNMENT ResourcePool {
public:
    // block 保存资源数目
    static const size_t BLOCK_NITEM = ResourcePoolBlockItemNum<T>::value;
    static const size_t FREE_CHUNK_NITEM = BLOCK_NITEM;

    // Free identifiers are batched in a FreeChunk before they're added to
    // global list(_free_chunks).
    // 定义Block中释放的 元素保存数组，保存的仅为ID值  一个 Block 释放元素ResourceID 索引
    typedef ResourcePoolFreeChunk<T, FREE_CHUNK_NITEM>      FreeChunk;
    // 柔性数组
    typedef ResourcePoolFreeChunk<T, 0> DynamicFreeChunk;

    // When a thread needs memory, it allocates a Block. To improve locality,
    // items in the Block are only used by the thread.
    // To support cache-aligned objects, align Block.items by cacheline.
    // __attribute__((aligned(n)))：此属性指定了指定类型的变量的最小对齐(以字节为单位)。如果结构中有成员的长度大于n，则按照最大成员的长度来对齐。
    struct BAIDU_CACHELINE_ALIGNMENT Block {
        char items[sizeof(T) * BLOCK_NITEM];
        size_t nitem; // 当前已使用的 索引位置

        Block() : nitem(0) {}
    };

    // BlockGroup  中包含多个 Block nblock为对应数量。全局只有一个

    // Block 包含 BLOCK_NITEM 个 T 结构， 每个线程中包含线程本地变量

    // A Resource addresses at most RP_MAX_BLOCK_NGROUP BlockGroups,
    // each BlockGroup addresses at most RP_GROUP_NBLOCK blocks. So a
    // resource addresses at most RP_MAX_BLOCK_NGROUP * RP_GROUP_NBLOCK Blocks.
    // RP_MAX_BLOCK_NGROUP 最多的BlockGroup个数
    struct BlockGroup {
        butil::atomic<size_t> nblock;
        // RP_GROUP_NBLOCK 65536
        butil::atomic<Block*> blocks[RP_GROUP_NBLOCK];

        BlockGroup() : nblock(0) {
            // We fetch_add nblock in add_block() before setting the entry,
            // thus address_resource() may sees the unset entry. Initialize
            // all entries to NULL makes such address_resource() return NULL.
            memset(blocks, 0, sizeof(butil::atomic<Block*>) * RP_GROUP_NBLOCK);
        }
    };


    // Each thread has an instance of this class.
    class BAIDU_CACHELINE_ALIGNMENT LocalPool {
    public:
        explicit LocalPool(ResourcePool* pool)
            : _pool(pool)
            , _cur_block(NULL)
            , _cur_block_index(0) {
            _cur_free.nfree = 0;
        }

        ~LocalPool() {
            // Add to global _free_chunks if there're some free resources
            if (_cur_free.nfree) {
                _pool->push_free_chunk(_cur_free);
            }

            _pool->clear_from_destructor_of_local_pool();
        }

        // 删除 localPool 需要传入 LocalPool地址，静态方法，
        // 提供实例化内部类删除自己的方法
        static void delete_local_pool(void* arg) {
            delete(LocalPool*)arg;
        }

        // We need following macro to construct T with different CTOR_ARGS
        // which may include parenthesis because when T is POD, "new T()"
        // and "new T" are different: former one sets all fields to 0 which
        // we don't want.
#define BAIDU_RESOURCE_POOL_GET(CTOR_ARGS)                              \
        /* Fetch local free id */                                  \
        /* 第一级 判断当前Block的 cur_free是否有释放的资源，未用参数实例化？ */                         \
        if (_cur_free.nfree) {                                          \
            /* 直接使用已释放的 资源ID， nfree-- */                                  \
            const ResourceId<T> free_id = _cur_free.ids[--_cur_free.nfree]; \
            *id = free_id;                                              \
            BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_SUB1;                   \
            return unsafe_address_resource(free_id);                    \
        }                                                               \
        /* Fetch a FreeChunk from global.                               \
           TODO: Popping from _free needs to copy a FreeChunk which is  \
           costly, but hardly impacts amortized performance. */         \
        /*  从全局的 free_chunk 中获取一个FreeChunk， 通过复制的方式覆盖本地的cur_free 成本过高。*/                                  \
        if (_pool->pop_free_chunk(_cur_free)) {                         \
            --_cur_free.nfree;                                          \
            const ResourceId<T> free_id =  _cur_free.ids[_cur_free.nfree]; \
            *id = free_id;                                              \
            BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_SUB1;                   \
            return unsafe_address_resource(free_id);                    \
        }
        /*  _cur_block 数量未超过 BLOCK_NITEM 直接从中new一个 */                 \
        /* Fetch memory from local block */                             \
        if (_cur_block && _cur_block->nitem < BLOCK_NITEM) {            \
            /* 计算ResourceID 值 ，当前 block的总索引 * BLOCK_NITEM  */   \
            id->value = _cur_block_index * BLOCK_NITEM + _cur_block->nitem; \
            T* p = new ((T*)_cur_block->items + _cur_block->nitem) T CTOR_ARGS; \
            if (!ResourcePoolValidator<T>::validate(p)) {               \
                p->~T();                                                \
                return NULL;                                            \
            }                                                           \
            ++_cur_block->nitem;                                        \
            return p;                                                   \
        }                                                               \
        /* Fetch a Block from global */                                 \
        /* 同时修改了 _cur_block 和 _cur_block_index */                                 \
        _cur_block = add_block(&_cur_block_index);                      \
        if (_cur_block != NULL) {                                       \
            id->value = _cur_block_index * BLOCK_NITEM + _cur_block->nitem; \
            T* p = new ((T*)_cur_block->items + _cur_block->nitem) T CTOR_ARGS; \
            if (!ResourcePoolValidator<T>::validate(p)) {               \
                p->~T();                                                \
                return NULL;                                            \
            }                                                           \
            ++_cur_block->nitem;                                        \
            return p;                                                   \
        }                                                               \
        return NULL;                                                    \
 

        // get 一个 资源 都是调用内部 宏实现，返回实例化的 资源的地址
        inline T* get(ResourceId<T>* id) {
            BAIDU_RESOURCE_POOL_GET();
        }

        template <typename A1>
        inline T* get(ResourceId<T>* id, const A1& a1) {
            BAIDU_RESOURCE_POOL_GET((a1));
        }

        template <typename A1, typename A2>
        inline T* get(ResourceId<T>* id, const A1& a1, const A2& a2) {
            BAIDU_RESOURCE_POOL_GET((a1, a2));
        }

#undef BAIDU_RESOURCE_POOL_GET

        inline int return_resource(ResourceId<T> id) {
            // Return to local free list
            if (_cur_free.nfree < ResourcePool::free_chunk_nitem()) {
                _cur_free.ids[_cur_free.nfree++] = id;
                BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_ADD1;
                return 0;
            }
            // Local free list is full, return it to global.
            // For copying issue, check comment in upper get()
            if (_pool->push_free_chunk(_cur_free)) {
                _cur_free.nfree = 1;
                _cur_free.ids[0] = id;
                BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_ADD1;
                return 0;
            }
            return -1;
        }

    private:
        // 指向资源池，内存不足是从该pool的 Block group 中获取。
        ResourcePool* _pool;
        // 当前线程本地 block
        Block* _cur_block;
        // 当前 block 在 BlockGroup 中的索引，包括了 BlockGroup 在 _block_groups 数组中的索引
        // 是摊平_block_groups的总索引，比如 当前block在 第3个 BlockGroup 的 第2个  Block，每个BlockGroup有8个 index ，则 _cur_block_index为 2*8+1
        size_t _cur_block_index;

        // 当前释放的_cur_free
        // 保存释放的资源T的ResourceId，可能是任何Block的释放的资源。 因为资源释放时会有稀疏内存，方便查找释放的资源重新分配
        FreeChunk _cur_free;
    };

    // 根据ID获得资源地址。
    // 不安全，未验证根据ResourceID计算出的索引是否溢出，速度快
    static inline T* unsafe_address_resource(ResourceId<T> id) {
        const size_t block_index = id.value / BLOCK_NITEM;
        return (T*)(_block_groups[(block_index >> RP_GROUP_NBLOCK_NBIT)]
                    .load(butil::memory_order_consume)
                    ->blocks[(block_index & (RP_GROUP_NBLOCK - 1))]
                    .load(butil::memory_order_consume)->items) +
               id.value - block_index * BLOCK_NITEM;
    }

    // 速度慢，判断移出
    static inline T* address_resource(ResourceId<T> id) {

        // 计算索引， id.value 为内存大小指标。例如 T为8 64/8=8
        const size_t block_index = id.value / BLOCK_NITEM;
        // ResourceId 前16位为group_index, 中间8位为 block_index，后8个为 block 中的索引
        const size_t group_index = (block_index >> RP_GROUP_NBLOCK_NBIT);

        if (__builtin_expect(group_index < RP_MAX_BLOCK_NGROUP, 1)) {

            // 获取 BlockGroup
            BlockGroup* bg =
                _block_groups[group_index].load(butil::memory_order_consume);
            if (__builtin_expect(bg != NULL, 1)) {
                // 获取 block
                Block* b = bg->blocks[block_index & (RP_GROUP_NBLOCK - 1)]
                           .load(butil::memory_order_consume);
                if (__builtin_expect(b != NULL, 1)) {
                    const size_t offset = id.value - block_index * BLOCK_NITEM;
                    if (__builtin_expect(offset < b->nitem, 1)) {
                        return (T*)b->items + offset;
                    }
                }
            }
        }

        return NULL;
    }

    // 获得一个新的资源，最终调用 LocalPool 的 get方法
    inline T* get_resource(ResourceId<T>* id) {
        LocalPool* lp = get_or_new_local_pool();
        if (__builtin_expect(lp != NULL, 1)) {
            return lp->get(id);
        }
        return NULL;
    }

    template <typename A1>
    inline T* get_resource(ResourceId<T>* id, const A1& arg1) {
        LocalPool* lp = get_or_new_local_pool();
        if (__builtin_expect(lp != NULL, 1)) {
            return lp->get(id, arg1);
        }
        return NULL;
    }

    template <typename A1, typename A2>
    inline T* get_resource(ResourceId<T>* id, const A1& arg1, const A2& arg2) {
        LocalPool* lp = get_or_new_local_pool();
        if (__builtin_expect(lp != NULL, 1)) {
            return lp->get(id, arg1, arg2);
        }
        return NULL;
    }

    inline int return_resource(ResourceId<T> id) {
        LocalPool* lp = get_or_new_local_pool();
        if (__builtin_expect(lp != NULL, 1)) {
            return lp->return_resource(id);
        }
        return -1;
    }

    void clear_resources() {
        LocalPool* lp = _local_pool;
        if (lp) {
            _local_pool = NULL;
            butil::thread_atexit_cancel(LocalPool::delete_local_pool, lp);
            delete lp;
        }
    }

    static inline size_t free_chunk_nitem() {
        const size_t n = ResourcePoolFreeChunkMaxItem<T>::value();
        return n < FREE_CHUNK_NITEM ? n : FREE_CHUNK_NITEM;
    }
    
    // Number of all allocated objects, including being used and free.
    ResourcePoolInfo describe_resources() const {
        ResourcePoolInfo info;
        info.local_pool_num = _nlocal.load(butil::memory_order_relaxed);
        info.block_group_num = _ngroup.load(butil::memory_order_acquire);
        info.block_num = 0;
        info.item_num = 0;
        info.free_chunk_item_num = free_chunk_nitem();
        info.block_item_num = BLOCK_NITEM;
#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
        info.free_item_num = _global_nfree.load(butil::memory_order_relaxed);
#endif

        for (size_t i = 0; i < info.block_group_num; ++i) {
            BlockGroup* bg = _block_groups[i].load(butil::memory_order_consume);
            if (NULL == bg) {
                break;
            }
            size_t nblock = std::min(bg->nblock.load(butil::memory_order_relaxed),
                                     RP_GROUP_NBLOCK);
            info.block_num += nblock;
            for (size_t j = 0; j < nblock; ++j) {
                Block* b = bg->blocks[j].load(butil::memory_order_consume);
                if (NULL != b) {
                    info.item_num += b->nitem;
                }
            }
        }
        info.total_size = info.block_num * info.block_item_num * sizeof(T);
        return info;
    }

    // 单例化 double check
    static inline ResourcePool* singleton() {
        // 读取
        ResourcePool* p = _singleton.load(butil::memory_order_consume);
        
        // 不存在
        if (p) {
            return p;
        }
        
        pthread_mutex_lock(&_singleton_mutex);
        p = _singleton.load(butil::memory_order_consume);
        if (!p) {
            //
            p = new ResourcePool();
            _singleton.store(p, butil::memory_order_release);
        } 
        pthread_mutex_unlock(&_singleton_mutex);
        return p;
    }

private:
    ResourcePool() {
        _free_chunks.reserve(RP_INITIAL_FREE_LIST_SIZE);
        pthread_mutex_init(&_free_chunks_mutex, NULL);
    }

    ~ResourcePool() {
        pthread_mutex_destroy(&_free_chunks_mutex);
    }

    // Create a Block and append it to right-most BlockGroup.
    static Block* add_block(size_t* index) {
        // 新建一个 block
        Block* const new_block = new(std::nothrow) Block;
        if (NULL == new_block) {
            return NULL;
        }

        size_t ngroup;
        do {
            ngroup = _ngroup.load(butil::memory_order_acquire);
            // 判断是否有需要增加 BlockGroup
            if (ngroup >= 1) {
                BlockGroup* const g =
                    _block_groups[ngroup - 1].load(butil::memory_order_consume);
                const size_t block_index =
                    g->nblock.fetch_add(1, butil::memory_order_relaxed);
                // 未超过每个GROUP_BLOCK 可存放block数，直接插入
                if (block_index < RP_GROUP_NBLOCK) {
                    g->blocks[block_index].store(
                        new_block, butil::memory_order_release);
                    //  前面_GROUP_BLOCK包括的 Block数加当前的 block_index，
                    *index = (ngroup - 1) * RP_GROUP_NBLOCK + block_index;
                    return new_block;
                }
                g->nblock.fetch_sub(1, butil::memory_order_relaxed);
            }
        } while (add_block_group(ngroup));

        // Fail to add_block_group.
        delete new_block;
        return NULL;
    }

    // Create a BlockGroup and append it to _block_groups.
    // Shall be called infrequently because a BlockGroup is pretty big.
    static bool add_block_group(size_t old_ngroup) {
        BlockGroup* bg = NULL;
        BAIDU_SCOPED_LOCK(_block_group_mutex);
        const size_t ngroup = _ngroup.load(butil::memory_order_acquire);
        if (ngroup != old_ngroup) {
            // Other thread got lock and added group before this thread.
            return true;
        }
        if (ngroup < RP_MAX_BLOCK_NGROUP) {
            bg = new(std::nothrow) BlockGroup;
            if (NULL != bg) {
                // Release fence is paired with consume fence in address() and
                // add_block() to avoid un-constructed bg to be seen by other
                // threads.
                _block_groups[ngroup].store(bg, butil::memory_order_release);
                _ngroup.store(ngroup + 1, butil::memory_order_release);
            }
        }
        return bg != NULL;
    }

    inline LocalPool* get_or_new_local_pool() {
        LocalPool* lp = _local_pool;
        if (lp != NULL) {
            return lp;
        }
        lp = new(std::nothrow) LocalPool(this);
        if (NULL == lp) {
            return NULL;
        }
        BAIDU_SCOPED_LOCK(_change_thread_mutex); //avoid race with clear()
        _local_pool = lp;
        butil::thread_atexit(LocalPool::delete_local_pool, lp);
        _nlocal.fetch_add(1, butil::memory_order_relaxed);
        return lp;
    }

    void clear_from_destructor_of_local_pool() {
        // Remove tls
        _local_pool = NULL;

        if (_nlocal.fetch_sub(1, butil::memory_order_relaxed) != 1) {
            return;
        }

        // Can't delete global even if all threads(called ResourcePool
        // functions) quit because the memory may still be referenced by 
        // other threads. But we need to validate that all memory can
        // be deallocated correctly in tests, so wrap the function with 
        // a macro which is only defined in unittests.
#ifdef BAIDU_CLEAR_RESOURCE_POOL_AFTER_ALL_THREADS_QUIT
        BAIDU_SCOPED_LOCK(_change_thread_mutex);  // including acquire fence.
        // Do nothing if there're active threads.
        if (_nlocal.load(butil::memory_order_relaxed) != 0) {
            return;
        }
        // All threads exited and we're holding _change_thread_mutex to avoid
        // racing with new threads calling get_resource().

        // Clear global free list.
        FreeChunk dummy;
        while (pop_free_chunk(dummy));

        // Delete all memory
        const size_t ngroup = _ngroup.exchange(0, butil::memory_order_relaxed);
        for (size_t i = 0; i < ngroup; ++i) {
            BlockGroup* bg = _block_groups[i].load(butil::memory_order_relaxed);
            if (NULL == bg) {
                break;
            }
            size_t nblock = std::min(bg->nblock.load(butil::memory_order_relaxed),
                                     RP_GROUP_NBLOCK);
            for (size_t j = 0; j < nblock; ++j) {
                Block* b = bg->blocks[j].load(butil::memory_order_relaxed);
                if (NULL == b) {
                    continue;
                }
                for (size_t k = 0; k < b->nitem; ++k) {
                    T* const objs = (T*)b->items;
                    objs[k].~T();
                }
                delete b;
            }
            delete bg;
        }

        memset(_block_groups, 0, sizeof(BlockGroup*) * RP_MAX_BLOCK_NGROUP);
#endif
    }

private:
    bool pop_free_chunk(FreeChunk& c) {
        // Critical for the case that most return_object are called in
        // different threads of get_object.

        // 加锁前检测
        if (_free_chunks.empty()) {
            return false;
        }
        pthread_mutex_lock(&_free_chunks_mutex);
        // 加锁后检测 _free_chunks， 防止加锁过程中被分配完
        if (_free_chunks.empty()) {
            pthread_mutex_unlock(&_free_chunks_mutex);
            return false;
        }
        DynamicFreeChunk* p = _free_chunks.back();
        _free_chunks.pop_back();
        pthread_mutex_unlock(&_free_chunks_mutex);
        c.nfree = p->nfree;
        // 执行拷贝，传递的是 C 因此只能拷贝覆盖原来的
        memcpy(c.ids, p->ids, sizeof(*p->ids) * p->nfree);
        free(p);
        return true;
    }

    bool push_free_chunk(const FreeChunk& c) {
        DynamicFreeChunk* p = (DynamicFreeChunk*)malloc(
            offsetof(DynamicFreeChunk, ids) + sizeof(*c.ids) * c.nfree);
        if (!p) {
            return false;
        }
        p->nfree = c.nfree;
        memcpy(p->ids, c.ids, sizeof(*c.ids) * c.nfree);
        pthread_mutex_lock(&_free_chunks_mutex);
        _free_chunks.push_back(p);
        pthread_mutex_unlock(&_free_chunks_mutex);
        return true;
    }
    
    static butil::static_atomic<ResourcePool*> _singleton;
    static pthread_mutex_t _singleton_mutex;
    static BAIDU_THREAD_LOCAL LocalPool* _local_pool;
    static butil::static_atomic<long> _nlocal;
    static butil::static_atomic<size_t> _ngroup;
    static pthread_mutex_t _block_group_mutex;
    static pthread_mutex_t _change_thread_mutex;

    // ResourcePool
    static butil::static_atomic<BlockGroup*> _block_groups[RP_MAX_BLOCK_NGROUP];
    // 保存了多个释放资源ID的 DynamicFreeChunk
    std::vector<DynamicFreeChunk*> _free_chunks;
    pthread_mutex_t _free_chunks_mutex;

#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
    static butil::static_atomic<size_t> _global_nfree;
#endif
};

// Declare template static variables:

template <typename T>
const size_t ResourcePool<T>::FREE_CHUNK_NITEM;

template <typename T>
BAIDU_THREAD_LOCAL typename ResourcePool<T>::LocalPool*
ResourcePool<T>::_local_pool = NULL;

template <typename T>
butil::static_atomic<ResourcePool<T>*> ResourcePool<T>::_singleton =
    BUTIL_STATIC_ATOMIC_INIT(NULL);

template <typename T>
pthread_mutex_t ResourcePool<T>::_singleton_mutex = PTHREAD_MUTEX_INITIALIZER;

template <typename T>
butil::static_atomic<long> ResourcePool<T>::_nlocal = BUTIL_STATIC_ATOMIC_INIT(0);

template <typename T>
butil::static_atomic<size_t> ResourcePool<T>::_ngroup = BUTIL_STATIC_ATOMIC_INIT(0);

template <typename T>
pthread_mutex_t ResourcePool<T>::_block_group_mutex = PTHREAD_MUTEX_INITIALIZER;

template <typename T>
pthread_mutex_t ResourcePool<T>::_change_thread_mutex =
    PTHREAD_MUTEX_INITIALIZER;

template <typename T>
butil::static_atomic<typename ResourcePool<T>::BlockGroup*>
ResourcePool<T>::_block_groups[RP_MAX_BLOCK_NGROUP] = {};

#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
template <typename T>
butil::static_atomic<size_t> ResourcePool<T>::_global_nfree = BUTIL_STATIC_ATOMIC_INIT(0);
#endif

template <typename T>
inline bool operator==(ResourceId<T> id1, ResourceId<T> id2) {
    return id1.value == id2.value;
}

template <typename T>
inline bool operator!=(ResourceId<T> id1, ResourceId<T> id2) {
    return id1.value != id2.value;
}

// Disable comparisons between different typed ResourceId
template <typename T1, typename T2>
bool operator==(ResourceId<T1> id1, ResourceId<T2> id2);

template <typename T1, typename T2>
bool operator!=(ResourceId<T1> id1, ResourceId<T2> id2);

inline std::ostream& operator<<(std::ostream& os,
                                ResourcePoolInfo const& info) {
    return os << "local_pool_num: " << info.local_pool_num
              << "\nblock_group_num: " << info.block_group_num
              << "\nblock_num: " << info.block_num
              << "\nitem_num: " << info.item_num
              << "\nblock_item_num: " << info.block_item_num
              << "\nfree_chunk_item_num: " << info.free_chunk_item_num
              << "\ntotal_size: " << info.total_size;
#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
              << "\nfree_num: " << info.free_item_num
#endif
           ;
}

}  // namespace butil

#endif  // BUTIL_RESOURCE_POOL_INL_H
