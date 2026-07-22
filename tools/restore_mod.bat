@echo off
echo === Restoring mod (removing UE4SS) ===

set GAME_DIR=D:\SteamLibrary\steamapps\common\Deadzone Rogue\Valhalla\Binaries\Win64

:: Remove UE4SS
if exist "%GAME_DIR%\ue4ss" (
    rmdir /S /Q "%GAME_DIR%\ue4ss"
    echo Removed ue4ss folder
)

:: Restore our mod
if exist "%GAME_DIR%\dwmapi_mod_backup.dll" (
    copy /Y "%GAME_DIR%\dwmapi_mod_backup.dll" "%GAME_DIR%\dwmapi.dll"
    del "%GAME_DIR%\dwmapi_mod_backup.dll"
    echo Restored mod DLL
) else (
    echo WARNING: No backup found, copying fresh build
    copy /Y "C:\Users\mike\Desktop\DeaderZoneRogue\mod\build\Release\dwmapi.dll" "%GAME_DIR%\dwmapi.dll"
)

echo.
echo === DONE - Mod restored ===
echo CXX headers should be at: %GAME_DIR%\CXXHeaderDump\
pause
