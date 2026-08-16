// comptr.h — 极简 COM 智能指针（仅系统 DLL，无第三方依赖）。
#pragma once
#include <unknwn.h>

namespace hsf {

template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    explicit ComPtr(T* p) : ptr_(p) {}
    ComPtr(const ComPtr& other)
    {
        Reset(other.ptr_);
        if (ptr_) ptr_->AddRef();
    }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ~ComPtr() { Reset(); }

    ComPtr& operator=(const ComPtr& other)
    {
        if (this != &other)
        {
            T* p = other.ptr_;
            if (p) p->AddRef();
            Reset(p);
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) { Reset(); ptr_ = other.ptr_; other.ptr_ = nullptr; }
        return *this;
    }
    ComPtr& operator=(T* p)
    {
        if (p) p->AddRef();
        Reset(p);
        return *this;
    }

    T* Get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T** GetAddressOf() { return &ptr_; }
    T*& operator&() = delete;

    void Reset(T* p = nullptr)
    {
        if (ptr_ && ptr_ != p) ptr_->Release();
        ptr_ = p;
    }

    template <typename U>
    HRESULT As(ComPtr<U>& out) const
    {
        return ptr_->QueryInterface(IID_PPV_ARGS(out.GetAddressOf()));
    }

    bool operator!() const { return ptr_ == nullptr; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

} // namespace hsf
