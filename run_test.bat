@echo off
REM Self-checking test suite for the LTRIM_ZERO collation.
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
echo Testing LTRIM_ZERO collation
echo ============================================
echo.

if exist test_ltrim.fdb del /q test_ltrim.fdb

"%ISQL%" -input test_ltrim_zero.sql

echo.
echo ============================================
echo Done. Check the SUMMARY and VERDICT above.
echo ============================================

endlocal
