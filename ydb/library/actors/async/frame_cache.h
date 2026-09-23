#pragma once

#include <util/system/yassert.h>

#include <array>
#include <cstddef>
#include <limits>
#include <new>

namespace NActors {

    // All access must be serialized. The cache must outlive every frame whose
    // allocation header references it, including lazy frames not yet awaited.
    // Uncached class-table overflow blocks have no owner and may outlive it.
    // Allocations guarantee __STDCPP_DEFAULT_NEW_ALIGNMENT__; extended
    // coroutine-frame alignment is not supported.
    class TAsyncFrameCache {
    public:
        static constexpr size_t MaxClasses = 64;
        static constexpr size_t MaxCachedPerClass = 64;

        struct TStats {
            size_t SizeClasses = 0;
            size_t LiveFrames = 0; // Cache-owned frames only; excludes uncached blocks.
            size_t CachedFrames = 0;
            size_t CachedBytes = 0;
            size_t HeapAllocations = 0;
        };

        TAsyncFrameCache() = default;
        TAsyncFrameCache(const TAsyncFrameCache&) = delete;
        TAsyncFrameCache& operator=(const TAsyncFrameCache&) = delete;
        TAsyncFrameCache(TAsyncFrameCache&&) = delete;
        TAsyncFrameCache& operator=(TAsyncFrameCache&&) = delete;

        ~TAsyncFrameCache() {
            Y_ABORT_UNLESS(!LiveFrames, "Coroutine frame cache destroyed with live frames");
            for (size_t i = 0; i < ClassCount; ++i) {
                auto* header = Classes[i].Head;
                while (header) {
                    auto* next = header->Next;
                    ::operator delete(header);
                    header = next;
                }
            }
        }

        void* Allocate(size_t size) {
            auto* sizeClass = FindClass(size);
            if (sizeClass && sizeClass->Head) {
                auto* header = sizeClass->Head;
                sizeClass->Head = header->Next;
                header->SizeClass = sizeClass;
                --sizeClass->Cached;
                ++LiveFrames;
                return header + 1;
            }

            if (!sizeClass && ClassCount == MaxClasses) {
                void* frame = AllocateUncached(size);
                ++HeapAllocations;
                return frame;
            }

            auto* header = AllocateBlock(size, this);
            if (!sizeClass) {
                sizeClass = &Classes[ClassCount++];
                sizeClass->Size = size;
            }
            header->SizeClass = sizeClass;
            ++HeapAllocations;
            ++LiveFrames;
            return header + 1;
        }

        static void* AllocateUncached(size_t size) {
            return AllocateBlock(size, nullptr) + 1;
        }

        // Does not consult the promise or actor: both may already be destroyed.
        static void Free(void* frame, size_t size) noexcept {
            auto* header = static_cast<THeader*>(frame) - 1;
            if (auto* cache = header->Cache) {
                auto* sizeClass = header->SizeClass;
                Y_ABORT_UNLESS(sizeClass && sizeClass->Size == size && cache->LiveFrames);
                --cache->LiveFrames;
                if (sizeClass->Cached < MaxCachedPerClass) {
                    header->Next = sizeClass->Head;
                    sizeClass->Head = header;
                    ++sizeClass->Cached;
                    return;
                }
            }
            ::operator delete(header);
        }

        // Computed on demand; no per-hit statistics on the allocation fast path.
        TStats GetStats() const noexcept {
            TStats stats{ClassCount, LiveFrames, 0, 0, HeapAllocations};
            for (size_t i = 0; i < ClassCount; ++i) {
                stats.CachedFrames += Classes[i].Cached;
                stats.CachedBytes += Classes[i].Cached * (sizeof(THeader) + Classes[i].Size);
            }
            return stats;
        }

    private:
        struct TSizeClass;

        struct alignas(__STDCPP_DEFAULT_NEW_ALIGNMENT__) THeader {
            TAsyncFrameCache* Cache;
            union {
                TSizeClass* SizeClass = nullptr; // Live block; Classes never move.
                THeader* Next; // Cached block.
            };
        };
        static_assert(sizeof(THeader) == (2 * sizeof(void*) + __STDCPP_DEFAULT_NEW_ALIGNMENT__ - 1)
            / __STDCPP_DEFAULT_NEW_ALIGNMENT__ * __STDCPP_DEFAULT_NEW_ALIGNMENT__);

        struct TSizeClass {
            size_t Size = 0;
            THeader* Head = nullptr;
            size_t Cached = 0;
        };

        static THeader* AllocateBlock(size_t size, TAsyncFrameCache* cache) {
            if (size > std::numeric_limits<size_t>::max() - sizeof(THeader)) {
                throw std::bad_alloc();
            }
            return ::new (::operator new(sizeof(THeader) + size)) THeader{cache, {nullptr}};
        }

        TSizeClass* FindClass(size_t size) noexcept {
            for (size_t i = 0; i < ClassCount; ++i) {
                if (Classes[i].Size == size) {
                    return &Classes[i];
                }
            }
            return nullptr;
        }

        std::array<TSizeClass, MaxClasses> Classes{};
        size_t ClassCount = 0;
        size_t LiveFrames = 0;
        size_t HeapAllocations = 0;
    };

} // namespace NActors
