#pragma once

#include "Renderer.hpp"

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wil/com.h>

#include <mutex>
#include <string>

namespace rmw::wallpaper
{

// The COM objects every monitor shares (Renderer.cpp makes them, Composer.cpp draws with them).
struct Graphics::Parts
{
    wil::com_ptr_nothrow<ID3D11Device> device;
    wil::com_ptr_nothrow<ID3D11DeviceContext> context;
    wil::com_ptr_nothrow<ID3D11VideoDevice> videoDevice;
    wil::com_ptr_nothrow<ID3D11VideoContext> videoContext;
    wil::com_ptr_nothrow<IMFDXGIDeviceManager> manager;
    UINT resetToken = 0;
    wil::com_ptr_nothrow<IDXGIFactory2> factory;
    wil::com_ptr_nothrow<IDCompositionDevice> composition;
    // Direct2D on the same device (M12: effects and particles); empty when it could not be made,
    // and then only plain videos play.
    wil::com_ptr_nothrow<ID2D1Factory1> d2dFactory;
    wil::com_ptr_nothrow<ID2D1Device> d2dDevice;
    // Direct2D's lock: held around our own Direct3D calls while another monitor may be drawing.
    wil::com_ptr_nothrow<ID2D1Multithread> d2dLock;
    std::string adapter;
    mutable std::mutex compositionLock; // DirectComposition calls, from the renderers' threads
};

// Direct2D's lock for as long as it lives (nothing when there is no Direct2D).
class D2DLock
{
public:
    explicit D2DLock(const Graphics::Parts& parts) noexcept
        : m_lock(parts.d2dLock.get())
    {
        if (m_lock != nullptr)
        {
            m_lock->Enter();
        }
    }
    ~D2DLock()
    {
        if (m_lock != nullptr)
        {
            m_lock->Leave();
        }
    }
    D2DLock(const D2DLock&) = delete;
    D2DLock& operator=(const D2DLock&) = delete;
    D2DLock(D2DLock&&) = delete;
    D2DLock& operator=(D2DLock&&) = delete;

private:
    ID2D1Multithread* m_lock;
};

} // namespace rmw::wallpaper
