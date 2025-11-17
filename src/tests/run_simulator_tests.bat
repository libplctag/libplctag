@echo off
REM ============================================================================
REM run_simulator_tests.bat
REM Windows batch equivalent of run_simulator_tests.sh
REM Tests the libplctag library with simulated AB and Modbus servers
REM ============================================================================

setlocal enabledelayedexpansion

REM Get the test directory from command line argument
if "%1"=="" (
    echo Error: Test directory not provided
    echo Usage: run_simulator_tests.bat ^<test_dir^>
    exit /b 1
)

set TEST_DIR=%1
set TEST=0
set SUCCESSES=0
set FAILURES=0

REM Verify test directory exists
if not exist "%TEST_DIR%" (
    echo %TEST_DIR% is not a valid path for test executables!
    exit /b 1
)

REM Check for required executables
setlocal enabledelayedexpansion
set EXECUTABLES=^
    ab_server.exe ^
    list_tags_logix.exe ^
    modbus_server.exe ^
    string_non_standard_udt.exe ^
    string_standard.exe ^
    stress_rc_mem.exe ^
    tag_rw2.exe ^
    test_auto_sync.exe ^
    test_callback.exe ^
    test_callback_ex.exe ^
    test_callback_ex_logix.exe ^
    test_callback_ex_modbus.exe ^
    test_indexed_tags.exe ^
    test_modbus_multiple.exe ^
    test_raw_cip.exe ^
    test_reconnect.exe ^
    test_reconnect_after_outage_async.exe ^
    test_reconnect_after_outage_sync.exe ^
    test_shutdown.exe ^
    test_special.exe ^
    test_string.exe ^
    test_tag_attributes.exe ^
    test_tag_type_attribute.exe ^
    thread_stress.exe

for %%E in (%EXECUTABLES%) do (
    if not exist "%TEST_DIR%\%%E" (
        echo %TEST_DIR%\%%E not found!
        exit /b 1
    )
)

echo.
echo ============================================================================
echo Starting AB emulator for fast ControlLogix tests.
echo ============================================================================
echo.

REM Start AB server for fast tests (non-blocking)
REM Use cmd /c to properly handle output redirection with start command
start "" cmd /c ""%TEST_DIR%\ab_server.exe" --debug --plc=ControlLogix --path=1,0 --tag=TestBigArray:DINT[2000] --tag=Test_Array_1:DINT[1000] --tag=Test_Array_2x3:DINT[2,3] --tag=Test_Array_2x3x4:DINT[2,3,4] > logix_fast_emulator.log 2>&1"

REM Give the server time to start and listen
timeout /T 3 /nobreak >nul

echo Checking that ab_server.exe is running...
tasklist /FI "IMAGENAME eq ab_server.exe" || echo ab_server.exe not found in tasklist

REM Test 1: basic large tag read/write
set /a TEST+=1
echo Test !TEST!: basic large tag read/write...
"%TEST_DIR%\tag_rw2.exe" --type=sint32 ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1000&name=TestBigArray" ^
    --debug=4 --write=1,2,3,4,5,6,7,8,9 ^
    > "!TEST!_big_tag_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 2: stress RC memory code
set /a TEST+=1
echo Test !TEST!: stress RC memory code ...
"%TEST_DIR%\stress_rc_mem.exe" > "!TEST!_stress_rc_mem_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 3: CIP thread stress
set /a TEST+=1
echo Test !TEST!: CIP thread stress...
"%TEST_DIR%\thread_stress.exe" 20 ^
    "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray" ^
    > "!TEST!_thread_stress_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 4: auto sync
set /a TEST+=1
echo Test !TEST!: auto sync...
"%TEST_DIR%\test_auto_sync.exe" > "!TEST!_auto_sync_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 5: indexed tags
set /a TEST+=1
echo Test !TEST!: indexed tags ...
"%TEST_DIR%\test_indexed_tags.exe" > "!TEST!_test_indexed_tags.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 6: hard library shutdown
set /a TEST+=1
echo Test !TEST!: hard library shutdown...
"%TEST_DIR%\test_shutdown.exe" > "!TEST!_shutdown.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

echo.
echo Killing AB emulator.
taskkill /F /IM ab_server.exe >nul 2>&1

echo.
echo ============================================================================
echo Starting stand-alone tests.
echo ============================================================================
echo.

REM Test 7: Test async reconnect after PLC outage
set /a TEST+=1
echo Test !TEST!: Test async reconnect after PLC outage...
"%TEST_DIR%\test_reconnect_after_outage_async.exe" "%TEST_DIR%\ab_server.exe" ^
    > "!TEST!_reconnect_after_outage_async.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 8: Test sync reconnect after PLC outage
set /a TEST+=1
echo Test !TEST!: Test sync reconnect after PLC outage...
"%TEST_DIR%\test_reconnect_after_outage_sync.exe" "%TEST_DIR%\ab_server.exe" ^
    > "!TEST!_reconnect_after_outage_sync.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

echo.
echo ============================================================================
echo Starting AB emulator for functional/slow ControlLogix tests.
echo ============================================================================
echo.

REM Start AB server for slow tests (with delay, non-blocking)
start "" cmd /c ""%TEST_DIR%\ab_server.exe" --plc=ControlLogix --path=1,0 --tag=TestBigArray:DINT[2000] --tag=Test_Array_1:DINT[1000] --tag=Test_Array_2x3:DINT[2,3] --tag=Test_Array_2x3x4:DINT[2,3,4] --delay=50 > logix_slow_emulator.log 2>&1"

REM Give the server time to start and listen
timeout /T 1 /nobreak >nul

REM Test 8: emulator test callbacks
set /a TEST+=1
echo Test !TEST!: emulator test callbacks...
"%TEST_DIR%\test_callback.exe" > "!TEST!_callback_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 9: emulator test extended callbacks sync
set /a TEST+=1
echo Test !TEST!: emulator test extended callbacks sync...
"%TEST_DIR%\test_callback_ex.exe" > "!TEST!_extended_callback_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 10: emulator test extended callbacks async
set /a TEST+=1
echo Test !TEST!: emulator test extended callbacks async...
"%TEST_DIR%\test_callback_ex_logix.exe" > "!TEST!_extended_callback_async_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

echo.
echo Killing AB emulator.
taskkill /F /IM ab_server.exe >nul 2>&1

echo.
echo ============================================================================
echo Starting AB emulator for Micro800 tests.
echo ============================================================================
echo.

REM Start Micro800 emulator (non-blocking)
start "" cmd /c ""%TEST_DIR%\ab_server.exe" --debug --plc=Micro800 --tag=TestDINTArray:DINT[10] > micro800_emulator.log 2>&1"

if errorlevel 1 (
    echo Unable to start Micro800 emulator!
    exit /b 1
)

timeout /T 1 /nobreak >nul

REM Test 11: basic Micro800 read/write
set /a TEST+=1
echo Test !TEST!: basic Micro800 read/write...
"%TEST_DIR%\tag_rw2.exe" --type=sint32 ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micro800&name=TestDINTArray" ^
    --write=42 --debug=4 ^
    > "!TEST!_micro800_tag_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

echo.
echo Killing Micro800 emulator.
taskkill /F /IM ab_server.exe >nul 2>&1

echo.
echo ============================================================================
echo Starting AB emulator for Omron tests.
echo ============================================================================
echo.

REM Start Omron emulator (non-blocking)
start "" cmd /c ""%TEST_DIR%\ab_server.exe" --debug --plc=Omron --tag=TestDINTArray:DINT[10] > omron_emulator.log 2>&1"

if errorlevel 1 (
    echo Unable to start AB/Omron emulator!
    exit /b 1
)

timeout /T 1 /nobreak >nul

REM Test 12: basic Omron read/write
set /a TEST+=1
echo Test !TEST!: basic Omron read/write...
"%TEST_DIR%\tag_rw2.exe" --type=sint32 ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray" ^
    --write=42 --debug=4 ^
    > "!TEST!_omron_tag_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

echo.
echo Killing Omron emulator.
taskkill /F /IM ab_server.exe >nul 2>&1

echo.
echo ============================================================================
echo Starting AB emulator for Micrologix tests.
echo ============================================================================
echo.

REM Start Micrologix emulator (non-blocking)
start "" cmd /c ""%TEST_DIR%\ab_server.exe" --debug --plc=Micrologix --tag=B3[10] --tag=N7[10] --tag=L19[10] > micrologix_emulator.log 2>&1"

if errorlevel 1 (
    echo Unable to start AB/Micrologix emulator!
    exit /b 1
)

timeout /T 1 /nobreak >nul

REM Test 13: B data file Micrologix tag read/write
set /a TEST+=1
echo Test !TEST!: B data file Micrologix tag read/write...
"%TEST_DIR%\tag_rw2.exe" --type=uint16 ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=B3:0" ^
    --write=0 --debug=4 ^
    > "!TEST!_micrologix.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 14: B bit data file Micrologix tag read/write
set /a TEST+=1
echo Test !TEST!: B bit data file Micrologix tag read/write...
"%TEST_DIR%\tag_rw2.exe" --type=bit ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=B3:0/6" ^
    --write=1 --debug=4 ^
    > "!TEST!_micrologix.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 15: N data file Micrologix tag read/write
set /a TEST+=1
echo Test !TEST!: N data file Micrologix tag read/write...
"%TEST_DIR%\tag_rw2.exe" --type=sint16 ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=N7:0" ^
    --write=42 --debug=4 ^
    > "!TEST!_micrologix.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 16: N bit data file Micrologix tag read/write
set /a TEST+=1
echo Test !TEST!: N bit data file Micrologix tag read/write...
"%TEST_DIR%\tag_rw2.exe" --type=bit ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=N7:0/10" ^
    --write=1 --debug=4 ^
    > "!TEST!_micrologix.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 17: L data file Micrologix tag read/write
set /a TEST+=1
echo Test !TEST!: L data file Micrologix tag read/write...
"%TEST_DIR%\tag_rw2.exe" --type=sint32 ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L10:0" ^
    --write=0,1,2,3 --debug=4 ^
    > "!TEST!_micrologix.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 18: L bit data file Micrologix tag read
set /a TEST+=1
echo Test !TEST!: L bit data file Micrologix tag read...
"%TEST_DIR%\tag_rw2.exe" --type=bit ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L10:0/23" ^
    --debug=4 ^
    > "!TEST!_micrologix.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 19: L bit data file Micrologix tag write
set /a TEST+=1
echo Test !TEST!: L bit data file Micrologix tag write...
"%TEST_DIR%\tag_rw2.exe" --type=bit ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L10:0/23" ^
    --write=1 --debug=4 ^
    > "!TEST!_micrologix.log" 2>&1
REM Note: This should NOT succeed, so invert the logic
if errorlevel 1 (
    echo OK
    set /a SUCCESSES+=1
) else (
    echo FAILURE
    set /a FAILURES+=1
)

echo.
echo Killing Micrologix emulator.
taskkill /F /IM ab_server.exe >nul 2>&1

echo.
echo ============================================================================
echo Starting AB emulator for PLC5 tests.
echo ============================================================================
echo.

REM Start PLC5 emulator (non-blocking)
start "" cmd /c ""%TEST_DIR%\ab_server.exe" --debug --plc=PLC/5 --tag=B3[10] --tag=N7[10] > plc5_emulator.log 2>&1"

if errorlevel 1 (
    echo Unable to start AB/PLC5 emulator!
    exit /b 1
)

timeout /T 1 /nobreak >nul

REM Test 20: B data file PLC5 tag read/write
set /a TEST+=1
echo Test !TEST!: B data file PLC5 tag read/write...
"%TEST_DIR%\tag_rw2.exe" --type=uint16 ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=B3:0" ^
    --debug=4 --write=0 ^
    > "!TEST!_plc5.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 21: B bit data file PLC5 tag read/write
set /a TEST+=1
echo Test !TEST!: B bit data file PLC5 tag read/write...
"%TEST_DIR%\tag_rw2.exe" --type=bit ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=B3:0/10" ^
    --debug=4 --write=1 ^
    > "!TEST!_plc5.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 22: N data file PLC5 tag read/write
set /a TEST+=1
echo Test !TEST!: N data file PLC5 tag read/write...
"%TEST_DIR%\tag_rw2.exe" --type=sint16 ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=N7:0" ^
    --debug=4 --write=0 ^
    > "!TEST!_plc5.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 23: N bit data file PLC5 tag read/write
set /a TEST+=1
echo Test !TEST!: N bit data file PLC5 tag read/write...
"%TEST_DIR%\tag_rw2.exe" --type=bit ^
    "--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=N7:0/10" ^
    --debug=4 --write=1 ^
    > "!TEST!_plc5.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

echo.
echo Killing emulators.
taskkill /F /IM ab_server.exe >nul 2>&1
taskkill /F /IM modbus_server.exe >nul 2>&1

timeout /T 3 /nobreak

echo.
echo ============================================================================
echo Starting Modbus server tests.
echo ============================================================================
echo.

REM Start Modbus server (non-blocking)
REM Pass entire command as a string to cmd /c to properly handle output redirection
start "" cmd /c ""%TEST_DIR%\modbus_server.exe" --listen=127.0.0.1:1502 --listen=127.0.0.1:2502 --debug=DETAIL > modbus_server.log 2>&1"

REM Give the process a moment to start
timeout /T 2 /nobreak >nul

REM Verify the process actually started
tasklist /FI "IMAGENAME eq modbus_server.exe" | find /I "modbus_server.exe" >nul
if errorlevel 1 (
    echo ERROR: Modbus server failed to start or crashed!
    echo.
    echo Modbus server output:
    type modbus_server.log
    exit /b 1
)

REM Give it a bit more time to fully bind to ports
timeout /T 1 /nobreak >nul

REM Test 24: test short reconnect with Modbus
set /a TEST+=1
echo Test !TEST!: test short reconnect with Modbus...
"%TEST_DIR%\test_reconnect.exe" 3 ^
    > "!TEST!_modbus_reconnect_short_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 25: test long reconnect with Modbus
set /a TEST+=1
echo Test !TEST!: test long reconnect with Modbus...
"%TEST_DIR%\test_reconnect.exe" 15 ^
    > "!TEST!_modbus_reconnect_long_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 26: thread stress Modbus
set /a TEST+=1
echo Test !TEST!: thread stress Modbus...
"%TEST_DIR%\thread_stress.exe" 10 ^
    "protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10" ^
    > "!TEST!_modbus_stress_test.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 27: callback events Modbus
set /a TEST+=1
echo Test !TEST!: callback events Modbus...
"%TEST_DIR%\test_callback_ex_modbus.exe" ^
    > "!TEST!_test_callback_ex_modbus.log" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 28: for Modbus reconnect bug
set /a TEST+=1
echo Test !TEST!: for Modbus reconnect bug...
set TST_LOG=!TEST!_modbus_reconnect_bug_test.log
"%TEST_DIR%\test_modbus_multiple.exe" ^
    > "!TST_LOG!" 2>&1
if errorlevel 1 (
    echo FAILURE
    set /a FAILURES+=1
) else (
    echo OK
    set /a SUCCESSES+=1
)

REM Test 29: check for exactly 2 PLC creation entries in Modbus reconnect test log
REM This validates proper PLC object reuse and no spurious creation/destruction.
set /a TEST+=1
echo Test !TEST!: check for exactly 2 PLC creation entries in Modbus reconnect test log...
setlocal enabledelayedexpansion
set PLC_COUNT=0
for /F %%A in ('findstr /C:"Creating new PLC." "!TST_LOG!" ^| find /C /V ""') do (
    set PLC_COUNT=%%A
)
if "!PLC_COUNT!"=="2" (
    echo OK (found !PLC_COUNT! PLC creation entries in log file !TST_LOG!)
    set /a SUCCESSES+=1
) else (
    echo FAILURE (expected 2 PLC creation entries, found !PLC_COUNT! in log file !TST_LOG!)
    set /a FAILURES+=1
)
endlocal enabledelayedexpansion

echo.
echo Killing Modbus emulator.
taskkill /F /IM modbus_server.exe >nul 2>&1

REM Make sure no ab_server instances are running
taskkill /F /IM ab_server.exe >nul 2>&1

timeout /T 2 /nobreak

echo.
echo ============================================================================
echo Results:
echo ============================================================================
echo.
echo %TEST% tests.
echo %SUCCESSES% successes.
echo %FAILURES% failures.
echo.

if %FAILURES% equ 0 (
    exit /b 0
) else (
    exit /b 1
)
