@echo off
rem Tripod sound GUI launcher: opens the published GUI (GitHub Pages) as an app window.
rem No local server is needed. Once opened, the GUI also works offline (service worker cache).
rem Keep this file ASCII only: cmd may misread UTF-8 Japanese text and break commands.
setlocal
set URL=https://yoshida256k.github.io/tripodbgm/

rem Open as an app window (no address bar). Chrome first, then Edge, then the default browser
set BROWSER=
for %%B in (
  "%ProgramFiles%\Google\Chrome\Application\chrome.exe"
  "%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe"
  "%LocalAppData%\Google\Chrome\Application\chrome.exe"
  "%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe"
  "%ProgramFiles%\Microsoft\Edge\Application\msedge.exe"
) do (
  if not defined BROWSER if exist %%B set BROWSER=%%B
)
if defined BROWSER (
  start "" %BROWSER% --app=%URL%
) else (
  start "" %URL%
)
