#include "uring_operation.h"

#include <util/system/compiler.h>
#include <util/system/yassert.h>

#include <limits>

namespace NKikimr::NPDisk {

TUringOperationBase::~TUringOperationBase() = default;

void TUringOperationBase::PrepareIov(void* buf, size_t size, ui64 offset) {
    Y_ABORT_UNLESS(size <= static_cast<ui64>(std::numeric_limits<i64>::max()));
    Y_ABORT_UNLESS(size <= std::numeric_limits<ui64>::max() - offset);
    TotalSize = size;
    DiskOffset = offset;
    ShortIoCount = 0;
    IsContinuation = false;
    ReadParts.clear();
    ReadCursors.clear();

#if defined(__linux__)
    Iov.clear();
    Iov.push_back({buf, size});
    IovBegin = 0;
    BytesProcessed = 0;
#else
    Y_UNUSED(buf);
#endif
}

void TUringOperationBase::PrepareReadParts(TConstArrayRef<TReadPart> parts) {
    Y_ABORT_UNLESS(!parts.empty());
    // The singleton keeps the existing scalar submission and completion path.
    // Copy it first because the supplied descriptors may belong to this object.
    const TReadPart first = parts.front();
    Y_ABORT_UNLESS(first.Size > 0);
    if (parts.size() == 1) {
        PrepareIov(first.Buffer, first.Size, first.DiskOffset);
        ReadParts.push_back(first);
        Result = 0;
        FixedBuffer = false;
        BufIndex = 0;
        NextReadPart = 0;
        RemainingReadParts = 0;
        ReadPartReachedKernel = false;
        return;
    }

    TStackVec<TReadPart, 1> descriptors(parts.begin(), parts.end());
    ReadParts = std::move(descriptors);
    ReadCursors.clear();
    ReadCursors.resize(parts.size());
    TotalSize = 0;
    DiskOffset = first.DiskOffset;
    ShortIoCount = 0;
    IsContinuation = false;
    NextReadPart = 0;
    RemainingReadParts = ReadParts.size();
    ReadPartReachedKernel = false;
    FixedBuffer = false;
    BufIndex = 0;
    Result = 0;
#if defined(__linux__)
    Iov.clear();
    IovBegin = 0;
    BytesProcessed = 0;
#endif
    for (size_t i = 0; i < ReadParts.size(); ++i) {
        const auto& part = ReadParts[i];
        Y_ABORT_UNLESS(part.Size > 0);
        Y_ABORT_UNLESS(part.Size <= static_cast<ui64>(std::numeric_limits<i64>::max()) - TotalSize);
        Y_ABORT_UNLESS(part.Size <= std::numeric_limits<ui64>::max() - part.DiskOffset);
        TotalSize += part.Size;
        auto& cursor = ReadCursors[i];
        cursor.Parent = this;
        cursor.DiskOffset = part.DiskOffset;
        cursor.Buffer = part.Buffer;
        cursor.Size = part.Size;
    }
}

i64 TUringOperationBase::GetReadPartResult(size_t index) const {
    Y_ABORT_UNLESS(index < ReadParts.size());
    return ReadParts.size() == 1 ? Result : ReadCursors[index].Result;
}

void TUringOperationBase::SetReadPartResult(size_t index, i64 result) {
    Y_ABORT_UNLESS(index < ReadParts.size());
    if (ReadParts.size() == 1) {
        Result = result;
    } else {
        ReadCursors[index].Result = result;
    }
}

#if defined(__linux__)
void TUringOperationBase::PrepareScatterGather(size_t count, ui64 offset) {
    Y_ABORT_UNLESS(count > 0 && count <= MAX_IOVS);

    TotalSize = 0;
    DiskOffset = offset;
    ShortIoCount = 0;
    IsContinuation = false;
    ReadParts.clear();
    ReadCursors.clear();

    Iov.clear();
    Iov.reserve(count);
    IovBegin = 0;
    BytesProcessed = 0;
}

void TUringOperationBase::AddIov(void* buf, size_t size) {
    Y_ABORT_UNLESS(Iov.size() < MAX_IOVS);
    Y_ABORT_UNLESS(size <= static_cast<ui64>(std::numeric_limits<i64>::max()) - TotalSize);
    Y_ABORT_UNLESS(size <= std::numeric_limits<ui64>::max() - DiskOffset - TotalSize);
    TotalSize += size;
    Iov.push_back({buf, size});
}
#endif

void TUringOperationBase::AdvanceIov(size_t bytesProcessed) {
    // On non-Linux there are no short reads/writes via io_uring, so NOP is fine.
#if defined(__linux__)
    Y_ABORT_UNLESS(bytesProcessed <= GetOperationBytes());
    Y_ABORT_UNLESS(bytesProcessed <= std::numeric_limits<ui64>::max() - DiskOffset);
    DiskOffset += bytesProcessed;
    BytesProcessed += bytesProcessed;

    // Consume whole iovecs, then trim the partial one at the new window start.
    size_t remaining = bytesProcessed;
    while (remaining > 0 && IovBegin < Iov.size()) {
        if (remaining >= Iov[IovBegin].iov_len) {
            remaining -= Iov[IovBegin].iov_len;
            ++IovBegin;
        } else {
            // Partial iovec: trim from the front.
            Iov[IovBegin].iov_base = static_cast<char*>(Iov[IovBegin].iov_base) + remaining;
            Iov[IovBegin].iov_len -= remaining;
            remaining = 0;
        }
    }
#else
    Y_UNUSED(bytesProcessed);
#endif
}

} // namespace NKikimr::NPDisk
