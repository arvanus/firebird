@echo off
::
:: Builds fbltrimzero.dll, a standalone Firebird INTL module carrying only the
:: LTRIM_ZERO collations, out of src/intl/ltrimzero/ld_min.cpp.
::
:: The module has no run time dependency on the Firebird libraries: the driver
:: allocates nothing, throws nothing and does not use ICU. It is linked against
:: the static CRT (/MT) on purpose, so that dropping it into a customer server
:: never drags in a Visual C++ redistributable. Nothing crosses the CRT
:: boundary: the engine owns every buffer the entry points touch.
::
:: Compile flags mirror the Release x64 settings of intl.vcxproj and
:: FirebirdCommon.props, including /EHsc- (FirebirdCommon.props:11), with
:: /MT as the one deliberate deviation (props use /MD). INTL_EXPORTS, which
:: intl.vcxproj defines, is left out on purpose: it is an unused define in
:: fbintl's build, not referenced by any .rc or source file, so this module
:: does not need it either.
::
:: Usage, from builds\win32, after setenvvar.bat:
::     make_ltrimzero.bat
::
:: Output: builds\win32\ltrimzero\fbltrimzero.dll
::
@echo on

@if "%FB_ROOT_PATH%"=="" (
    @echo Run setenvvar.bat first.
    @exit /b 1
)

@set LTZ_OUT=%~dp0ltrimzero
@if not exist "%LTZ_OUT%" mkdir "%LTZ_OUT%"

cl /nologo ^
   /O2 /MT /GR- /EHsc- /std:c++17 /W3 ^
   /D NDEBUG /D _WINDOWS /D _USRDLL /D WINDOWS_ONLY /D SUPERCLIENT ^
   /D WIN32 /D _CRT_SECURE_NO_WARNINGS ^
   /I "%FB_ROOT_PATH%\src" ^
   /I "%FB_ROOT_PATH%\src\include" ^
   /I "%FB_ROOT_PATH%\src\include\gen" ^
   /I "%FB_ROOT_PATH%\src\jrd" ^
   /LD "%FB_ROOT_PATH%\src\intl\ltrimzero\ld_min.cpp" ^
   /Fo"%LTZ_OUT%\\" /Fe"%LTZ_OUT%\fbltrimzero.dll" ^
   /link /OPT:REF /OPT:ICF

@if errorlevel 1 (
    @echo BUILD FAILED
    @exit /b 1
)

@copy /Y "%FB_ROOT_PATH%\src\intl\ltrimzero\fbltrimzero.conf" "%LTZ_OUT%\" >nul

@echo.
@echo --- exported entry points
dumpbin /nologo /exports "%LTZ_OUT%\fbltrimzero.dll" | findstr /C:"LD_version" /C:"LD_lookup_texttype_with_status"
@echo.
@echo --- dependencies
dumpbin /nologo /dependents "%LTZ_OUT%\fbltrimzero.dll"
