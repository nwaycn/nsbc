@echo off
REM Compile NSBC Event Client (Java 8+, pure stdlib)
REM Usage: build.bat

setlocal
cd /d %~dp0

set SRC=src
set OUT=out

if not exist %SRC%\nsbc\NsbcClient.java (
    echo [FAIL] NsbcClient.java not found. Run from examples/event_client/java\
    exit /b 1
)

if exist %OUT% rmdir /s /q %OUT%
mkdir %OUT%

where javac >nul 2>nul
if errorlevel 1 (
    echo [FAIL] javac not found. Install JDK 8^+ (JDK, not JRE).
    exit /b 1
)

echo [compile] javac ...
javac -encoding UTF-8 -source 1.8 -target 1.8 -d %OUT% %SRC%\nsbc\NsbcClient.java %SRC%\nsbc\EventClient.java
if errorlevel 1 (
    echo [FAIL] compile error.
    exit /b 1
)

echo [OK] compiled to %OUT%\nsbc\
echo      run: java -cp %OUT% nsbc.EventClient --help
endlocal
