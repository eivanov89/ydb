#pragma once

#include <ydb/library/actors/util/rope.h>
#include <util/generic/array_ref.h>
#include <cstring>
#include <variant>
#include <vector>

namespace NKikimr::NDDisk {

// Views are transient: never retain one across a move or coroutine suspension.
class TReadChecksums {
    std::variant<std::monostate, ui64, std::vector<ui64>> Storage;
public:
    TConstArrayRef<ui64> View() const {
        if (const auto* single = std::get_if<ui64>(&Storage)) {
            return {single, 1};
        }
        if (const auto* many = std::get_if<std::vector<ui64>>(&Storage)) {
            return *many;
        }
        return {};
    }

    void SetSingle(ui64 value) {
        Storage = value;
    }

    void SetMany(std::vector<ui64>&& values) {
        if (values.empty()) {
            Clear();
        }
        else if (values.size() == 1) {
            SetSingle(values.front());
        }
        else {
            Storage = std::move(values);
        }
    }

    void Clear() {
        Storage.emplace<std::monostate>();
    }
};

// Native buffers remain native until the client reply needs a rope.
class TReadPayload {
    std::variant<std::monostate, TRcBuf, TRope> Storage;
public:
    TReadPayload() = default;

    TReadPayload(TRcBuf data) : Storage(std::move(data)) {
    }

    TReadPayload(TRope data) : Storage(std::move(data)) {
    }

    bool IsNative() const {
        return std::holds_alternative<TRcBuf>(Storage);
    }

    size_t size() const {
        if (const auto* data = std::get_if<TRcBuf>(&Storage)) {
            return data->size();
        }
        if (const auto* data = std::get_if<TRope>(&Storage)) {
            return data->size();
        }
        return 0;
    }

    TArrayRef<char> MutableSpan() {
        if (auto* data = std::get_if<TRcBuf>(&Storage)) {
            return {data->GetDataMut(), data->size()};
        }
        if (auto* data = std::get_if<TRope>(&Storage)) {
            return data->UnsafeGetContiguousSpanMut();
        }
        return {};
    }

    void CopyTo(void* destination, size_t size) const {
        Y_ABORT_UNLESS(size == this->size());
        if (const auto* data = std::get_if<TRcBuf>(&Storage)) {
            memcpy(destination, data->GetData(), size);
        }
        else if (const auto* data = std::get_if<TRope>(&Storage)) {
            data->Begin().ExtractPlainDataAndAdvance(destination, size);
        }
    }

    TRope IntoRope() && {
        TRope result;
        if (auto* data = std::get_if<TRcBuf>(&Storage)) {
            result = TRope(std::move(*data));
        }
        else if (auto* data = std::get_if<TRope>(&Storage)) {
            result = std::move(*data);
        }
        Storage.emplace<std::monostate>();
        return result;
    }
};

} // namespace NKikimr::NDDisk
