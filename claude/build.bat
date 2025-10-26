@echo off
echo Setting up Visual Studio environment...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

if errorlevel 1 (
    echo ERROR: vcvarsall.bat failed
    exit /b 1
)

echo.
echo Configuring CMake...
"C:\Users\ritte\AppData\Local\Programs\CLion 2\bin\cmake\win\x64\bin\cmake.exe" -S . -B cmake-build-debug -G "NMake Makefiles"

if errorlevel 1 (
    echo ERROR: CMake configuration failed
    exit /b 1
)

echo.
echo Building project...
"C:\Users\ritte\AppData\Local\Programs\CLion 2\bin\cmake\win\x64\bin\cmake.exe" --build cmake-build-debug --target unit_tests

if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo Build successful!
echo.
echo Running unit tests...
cmake-build-debug\unit_tests.exe

if errorlevel 1 (
    echo.
    echo TESTS FAILED!
    exit /b 1
)

echo.
echo All tests passed!
