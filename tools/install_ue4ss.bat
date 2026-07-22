@echo off
echo === Installing UE4SS (temporarily replacing mod) ===

set GAME_DIR=D:\SteamLibrary\steamapps\common\Deadzone Rogue\Valhalla\Binaries\Win64
set UE4SS_DIR=C:\Users\mike\Desktop\DeaderZoneRogue\tools\UE4SS_extracted

:: Back up our mod
if exist "%GAME_DIR%\dwmapi.dll" (
    copy /Y "%GAME_DIR%\dwmapi.dll" "%GAME_DIR%\dwmapi_mod_backup.dll"
    echo Backed up mod DLL
)

:: Copy UE4SS dwmapi.dll
copy /Y "%UE4SS_DIR%\dwmapi.dll" "%GAME_DIR%\dwmapi.dll"
echo Installed UE4SS dwmapi.dll

:: Copy ue4ss folder
if not exist "%GAME_DIR%\ue4ss" mkdir "%GAME_DIR%\ue4ss"
xcopy /E /Y /Q "%UE4SS_DIR%\ue4ss\*" "%GAME_DIR%\ue4ss\"
echo Installed ue4ss folder

echo.
echo === DONE ===
echo Launch the game now.
echo Once in-game (main menu or a run), press Ctrl+H to generate CXX headers.
echo Headers will be saved to: %GAME_DIR%\CXXHeaderDump\
echo After dumping, close the game and run restore_mod.bat
pause
