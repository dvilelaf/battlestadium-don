@echo off
rem Windows launcher (Tk language selector); adapted from the f7gs project's launch.bat. Cancel does not start the game.
cd /d "%~dp0..\.."
where py >nul 2>nul && (py -3 -I tools\launcher\launch.py %* & goto :eof)
python -I tools\launcher\launch.py %*
