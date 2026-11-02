@echo off
rem Starts Battle Stadium D.O.N. Asks nothing. Prints the commit this folder was built from (BUILT_FROM.txt).
rem The first line of game_folder.txt is the folder with your own copy of the game's files (the one that
rem holds SLPS_256.75). A folder dragged onto this file, or given as the first argument, is used instead.
rem Anything after the folder goes to the program (for example --sound off, --renderer software).
setlocal
cd /d "%~dp0"
if exist BUILT_FROM.txt findstr /b /c:"commit " /c:"subject " BUILT_FROM.txt
set "GAME="
set "A=%~1"
if defined A if not "%A:~0,1%"=="-" (
  set "GAME=%A%"
  shift
)
if not defined GAME (
  for /f "usebackq delims=" %%L in ("game_folder.txt") do (
    if not defined GAME (
      echo %%L| findstr /b /c:"#" >nul || set "GAME=%%L"
    )
  )
)
if not defined GAME goto :nogame
if not exist "%GAME%\SLPS_256.75" goto :nogame
if not exist "%~dp0saves" mkdir "%~dp0saves"
if not defined DON_GS_DATA_DIR set "DON_GS_DATA_DIR=%LOCALAPPDATA%\don\gs-cache"
if not exist "%DON_GS_DATA_DIR%" mkdir "%DON_GS_DATA_DIR%"
don.exe "%GAME%\SLPS_256.75" --mc-dir "%~dp0saves" %1 %2 %3 %4 %5 %6 %7 %8 %9
if errorlevel 1 pause
exit /b %errorlevel%
:nogame
echo Cannot find SLPS_256.75. Write the folder that holds your game's files on the first line of
echo game_folder.txt (next to this file), or drop that folder onto start.bat.
echo Read README.txt.
pause
exit /b 2
