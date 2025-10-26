# Build Setup - Visual Studio Paths for System PATH

## Required Paths for Building from Command Line

To build the project from command line (outside CLion), add these paths to your **System Environment Variables PATH**:

### 1. MSVC Compiler Tools (REQUIRED)
Contains: cl.exe, nmake.exe, link.exe, lib.exe

```
C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64
```

### 2. Windows SDK Tools (REQUIRED)
Contains: rc.exe, mt.exe, and other SDK utilities

```
C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64
```

### 3. MSBuild (Optional)
Contains: msbuild.exe (useful for solution builds)

```
C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin
```

## How to Add to PATH

1. Press `Win + X` and select "System"
2. Click "Advanced system settings"
3. Click "Environment Variables"
4. Under "System variables" (or "User variables"), find "Path"
5. Click "Edit"
6. Click "New" for each path above
7. Click "OK" to save
8. **Restart your terminal/IDE** for changes to take effect

## Verification

After adding to PATH and restarting your terminal, verify with:

```bash
where nmake
where cl
where link
```

These should now show the paths to the tools.

## Alternative: Developer Command Prompt

Instead of modifying PATH permanently, you can use "Developer Command Prompt for VS 2022" from the Start Menu, which automatically sets up all necessary paths for each session.

## Build Commands

Once PATH is configured:

```bash
# Using CMake
cmake --build cmake-build-debug --target example_usage

# Or directly with nmake
cd cmake-build-debug
nmake example_usage
```

## Notes

- The MSVC version `14.44.35207` may change with Visual Studio updates
- The Windows SDK version `10.0.26100.0` is the latest available on this system
- These paths are for x64 (64-bit) builds
