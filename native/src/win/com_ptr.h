// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tiny COM smart pointers. Deliberately not built on top of ATL/WIL so the
// addon has no dependencies outside the Windows SDK and node's N-API headers.

#pragma once

#include <objbase.h>
#include <unknwn.h>

#include <atomic>
#include <cstddef>
#include <utility>

namespace saf::win {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    explicit ComPtr(T* ptr) : ptr_(ptr) {}

    ~ComPtr() { reset(); }

    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    /** Address of the internal pointer, for out parameters. */
    T** put() {
        reset();
        return &ptr_;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    T* detach() {
        T* ptr = ptr_;
        ptr_ = nullptr;
        return ptr;
    }

private:
    T* ptr_ = nullptr;
};

/** Minimal reference counted COM object base for our callback objects. */
template <typename T, const IID* InterfaceId>
class ComCallback : public T {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == *InterfaceId) {
            *ppvObject = static_cast<T*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --refCount_;
        if (count == 0) delete this;
        return count;
    }

protected:
    virtual ~ComCallback() = default;

private:
    std::atomic<ULONG> refCount_ { 1 };
};

} // namespace saf::win
