#pragma once

#include "rmw/base/Error.hpp"

#include <string>

namespace rmw::ports
{

// The lock screen's picture, through Windows' own API (Windows.System.UserProfile.LockScreen: a
// desktop app may call it, spike S9). Per user. Setting a picture turns Windows Spotlight off by
// itself (HKCU\...\ContentDeliveryManager RotatingLockScreenEnabled = 0).
class ILockScreen
{
public:
    virtual ~ILockScreen() = default;

    // A file with the picture shown now: the one it was set from while Windows still names it and
    // it is there, else Windows' own copy saved to a file of ours (in %TEMP%).
    [[nodiscard]] virtual Result<std::string> currentImage() = 0;
    // The file Windows says the picture was set from ("" when it names none). Cheap.
    [[nodiscard]] virtual Result<std::string> imageSource() = 0;
    // A .jpg or .png the caller checked.
    [[nodiscard]] virtual Result<void> setImage(const std::string& file) = 0;
    // Whether a file is there (a captured picture may have been deleted since).
    [[nodiscard]] virtual bool exists(const std::string& file) = 0;
};

} // namespace rmw::ports
