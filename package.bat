@echo off
setlocal enabledelayedexpansion

echo ====================================================
echo Packaging V-Open for Distribution
echo ====================================================

set DIST_DIR=V-Open_Release
if exist "%DIST_DIR%" (
    echo Cleaning existing build directory...
    rmdir /s /q "%DIST_DIR%"
)
mkdir "%DIST_DIR%"

echo Copying binaries and dependencies...

REM Find VStudio.exe
set EXE_PATH=
if exist "cmake-build-release\VStudio.exe" (
    set EXE_PATH=cmake-build-release\VStudio.exe
    set SRC_DIR=cmake-build-release
) else if exist "cmake-build-debug\VStudio.exe" (
    set EXE_PATH=cmake-build-debug\VStudio.exe
    set SRC_DIR=cmake-build-debug
) else (
    echo ERROR: VStudio.exe not found! Please build the project first.
    exit /b 1
)

copy "%EXE_PATH%" "%DIST_DIR%\"
if exist "%SRC_DIR%\*.dll" (
    copy "%SRC_DIR%\*.dll" "%DIST_DIR%\"
)

echo Copying shaders...
if exist "shaders" (
    xcopy /e /i /y "shaders" "%DIST_DIR%\shaders"
)

echo Copying Python tracker script...
if exist "Tracker_VideoProcessing" (
    mkdir "%DIST_DIR%\Tracker_VideoProcessing"
    copy "Tracker_VideoProcessing\main.py" "%DIST_DIR%\Tracker_VideoProcessing\"
)

echo Creating run.bat launcher...
(
echo @echo off
echo echo Starting V-Open Launcher...
echo.
echo REM Check Python installation
echo where python ^>nul 2^>^&1
echo if %%ERRORLEVEL%% neq 0 (
echo     echo Python not found on the system.
echo     echo Attempting to install Python via winget...
echo     winget install -e --id Python.Python.3.10
echo     if %%ERRORLEVEL%% neq 0 (
echo         echo Failed to install Python via winget. Please install Python 3.10+ manually.
echo         pause
echo         exit /b 1
echo     )
echo     echo Please restart this script after the Python installation completes.
echo     pause
echo     exit /b 0
echo )
echo.
echo REM Check if virtual environment exists
echo if not exist "Tracker_VideoProcessing\.venv" (
echo     echo Creating Python virtual environment...
echo     python -m venv Tracker_VideoProcessing\.venv
echo     if %%ERRORLEVEL%% neq 0 (
echo         echo Failed to create virtual environment.
echo         pause
echo         exit /b 1
echo     )
echo     echo Installing Python tracking requirements...
echo     Tracker_VideoProcessing\.venv\Scripts\pip.exe install opencv-python mediapipe websockets numpy
echo     if %%ERRORLEVEL%% neq 0 (
echo         echo Failed to install Python dependencies.
echo         pause
echo         exit /b 1
echo     )
echo )
echo.
echo echo Launching V-Open...
echo start VStudio.exe
) > "%DIST_DIR%\run.bat"

echo.
echo ====================================================
echo V-Open packaged successfully in directory: %DIST_DIR%
echo Run 'run.bat' inside %DIST_DIR% on other machines.
echo ====================================================
pause
