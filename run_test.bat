@echo off
REM Self-checking test suite for the ID_ZPAD_CI collation.
REM The suite passes when the report ends with *** SUITE PASSED *** and FAILED = 0.

setlocal

cd /d D:\GitHub\firebird

set FIREBIRD=D:\GitHub\firebird\temp\x64\Release\firebird
set ISQL=%FIREBIRD%\isql.exe

if not exist "%ISQL%" (
    echo isql not found at %ISQL%
    echo Build first, then run this script again.
    exit /b 1
)

echo ============================================
echo Testing ID_ZPAD_CI collation
echo ============================================
echo.

if exist test_ltrim.fdb del /q test_ltrim.fdb

"%ISQL%" -input test_id_zpad_ci.sql

echo.
echo ============================================
echo Done. Check the SUMMARY and VERDICT above.
echo ============================================

endlocal
