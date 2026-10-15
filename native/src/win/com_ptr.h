// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tiny COM smart pointers and a minimal *agile* callback base. Deliberately not
// built on top of ATL/WIL so the addon has no dependencies outside the Windows
// SDK and node's N-API headers.
//
// ---------------------------------------------------------------------------
// Why the callback base implements IAgileObject
// ---------------------------------------------------------------------------
//
// Windows calls IActivateAudioInterfaceCompletionHandler::ActivateCompleted from
// a worker thread in the COM multi threaded apartment, no matter which apartment
// the caller of ActivateAudioInterfaceAsync lives in. Handing a plain reference
// counted object to that API violates the COM threading rules, and the result is
//
//     E_ILLEGAL_METHOD_CALL (0x8000000E)
//
// returned by ActivateAudioInterfaceAsync *itself*: the request never reaches
// the audio engine, so the error says nothing about the Windows build, the
// process, or process loopback support. Microsoft documents both the failure and
// the cure ("the completionHandler passed into ActivateAudioInterfaceAsync needs
// to implement IAgileObject", or equivalently aggregate the free threaded
// marshaler). This is exactly what the plugin produced on Windows 10 19045
// before this file changed, and it is why a bare HRESULT was so misleading.
//
// The cure implemented here is the minimal, well defined half of the documented
// requirement: the object reports IAgileObject, so COM's standard marshaler
// takes its "agile" fast path and passes the real pointer to the MTA instead of
// building a proxy that can never work. The free threaded marshaler variant
// (aggregating CLSID_FreeThreadedMarshaler) is deliberately *not* used: that
// aggregation makes the marshaler hold a reference to the outer object, which
// turns the pair into a reference cycle that leaks one handler per activation —
// and a capture restart performs one activation per application.

#pragma once

#include <objbase.h>
#include <unknwn.h>

#include <atomic>
#include <cstddef>
#include <utility>

namespace saf::win {

// IAgileObject — {94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90}. A marker interface with
// no methods: answering it promises "this pointer may be used from any
// apartment". Defined here (selectany, so it cannot collide with a copy from
// uuid.lib) because the SDK headers only declare it.
#ifndef DECLSPEC_SELECTANY
#    define DECLSPEC_SELECTANY __declspec(selectany)
#endif
EXTERN_C const GUID DECLSPEC_SELECTANY IID_IAgileObject = {
    0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 }
};

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

/**
 * Minimal reference counted COM object base for our callbacks.
 *
 * The object is agile: an apartment agnostic callback is a hard requirement of
 * ActivateAudioInterfaceAsync (see the file comment above) and it is also what
 * makes the audio session notification callback boundary safe.
 */
template <typename T, const IID* InterfaceId>
class ComCallback : public T {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;

        if (riid == IID_IUnknown || riid == *InterfaceId || riid == IID_IAgileObject) {
            *ppvObject = static_cast<T*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --refCount_;
        if (count == 0) delete this;
        return count;
    }

    /** Diagnostics: does the object really answer IAgileObject? */
    bool isAgile() const {
        ComCallback* self = const_cast<ComCallback*>(this);
        void* agile = nullptr;
        if (FAILED(self->QueryInterface(IID_IAgileObject, &agile)) || !agile) return false;
        static_cast<IUnknown*>(agile)->Release();
        return true;
    }

protected:
    virtual ~ComCallback() = default;

private:
    std::atomic<ULONG> refCount_ { 1 };
};

} // namespace saf::win
