## Why This Copy Is Trimmed

This folder is a narrowed vendor copy of upstream JoyShockLibrary for Crimson Weather.

The goal is not to ship the whole upstream project as-is. The goal is to keep only the pieces that look useful for a future Sony-controller backend while avoiding stale build artifacts and unrelated tooling.

## Kept

- `LICENSE.md`
- `README.md`
- `JoyShockLibrary/JoyShockLibrary.h`
- `JoyShockLibrary/JoyShockLibrary.cpp`
- `JoyShockLibrary/JoyShock.cpp`
- `JoyShockLibrary/InputHelpers.cpp`
- `JoyShockLibrary/tools.cpp`
- `JoyShockLibrary/GamepadMotion.hpp`
- `JoyShockLibrary/hidapi/hidapi.h`
- `JoyShockLibrary/hidapi/windows/hid.c`
- `JoyShockLibrary/hidapi/LICENSE.txt`

## Removed

- solution / project files
- CMake glue
- scripts
- Pascal binding
- Visual Studio precompiled-header files
- prebuilt `hid.obj` binaries
- bundled `hidapi-master.zip`
- extracted temporary `hidapi-master/` tree

## Important Notes

- Upstream JoyShockLibrary is viable because it includes real Windows HID I/O.
- It is still thread-heavy and callback-heavy, so this should not be dropped blindly into the mod runtime.
- If we integrate it, the intended use is a narrow Sony-only backend while leaving the existing XInput path untouched.
- `hidapi/windows/hid.c` was taken from the bundled `hidapi-master.zip` so we have real source instead of opaque prebuilt objects.
