# ============================================================================
# run_simulator_tests.ps1
# PowerShell script for testing libplctag library with simulated servers
# Usage: .\run_simulator_tests.ps1 <test_dir>
# ============================================================================

param(
    [Parameter(Mandatory=$true)]
    [string]$TestDir
)

$ErrorActionPreference = "Stop"

# Initialize counters
$script:TestCount = 0
$script:Successes = 0
$script:Failures = 0

# Verify test directory exists
if (-not (Test-Path $TestDir)) {
    Write-Error "$TestDir is not a valid path for test executables!"
    exit 1
}

# Check for required executables
$RequiredExecutables = @(
    'ab_server.exe',
    'list_tags_logix.exe',
    'modbus_server.exe',
    'string_non_standard_udt.exe',
    'string_standard.exe',
    'stress_rc_mem.exe',
    'tag_rw2.exe',
    'test_auto_sync.exe',
    'test_callback.exe',
    'test_callback_ex.exe',
    'test_callback_ex_logix.exe',
    'test_callback_ex_modbus.exe',
    'test_indexed_tags.exe',
    'test_modbus_multiple.exe',
    'test_raw_cip.exe',
    'test_reconnect.exe',
    'test_reconnect_after_outage_async.exe',
    'test_reconnect_after_outage_sync.exe',
    'test_shutdown.exe',
    'test_special.exe',
    'test_string.exe',
    'test_tag_attributes.exe',
    'test_tag_type_attribute.exe',
    'thread_stress.exe'
)

foreach ($exe in $RequiredExecutables) {
    $path = Join-Path $TestDir $exe
    if (-not (Test-Path $path)) {
        Write-Error "$path not found!"
        exit 1
    }
}

# Helper function to run a test
function Run-Test {
    param(
        [string]$Name,
        [string]$Executable,
        [string[]]$Arguments,
        [string]$LogFile
    )
    
    $script:TestCount++
    Write-Host "Test ${script:TestCount}: $Name..."
    
    $exePath = Join-Path $TestDir $Executable
    
    # PowerShell doesn't allow stdout and stderr to go to same file with Start-Process
    # Use separate files and merge them after
    $stdoutLog = $LogFile
    $stderrLog = $LogFile -replace '\.log$', '_err.log'
    
    $process = Start-Process -FilePath $exePath -ArgumentList $Arguments `
        -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog `
        -NoNewWindow -Wait -PassThru
    
    # Merge stderr into main log file if it exists
    if (Test-Path $stderrLog) {
        if ((Get-Item $stderrLog).Length -gt 0) {
            Add-Content -Path $stdoutLog -Value "`n--- STDERR ---"
            Get-Content $stderrLog | Add-Content -Path $stdoutLog
        }
        Remove-Item $stderrLog -Force -ErrorAction SilentlyContinue
    }
    
    if ($process.ExitCode -eq 0) {
        Write-Host "OK" -ForegroundColor Green
        $script:Successes++
    } else {
        Write-Host "FAILURE" -ForegroundColor Red
        $script:Failures++
    }
}

# Helper function to start a server in background
function Start-Server {
    param(
        [string]$Name,
        [string]$Executable,
        [string[]]$Arguments,
        [string]$LogFile,
        [switch]$Critical = $false
    )
    
    $exePath = Join-Path $TestDir $Executable
    
    try {
        # PowerShell doesn't allow stdout and stderr to go to same file with Start-Process
        # Use separate files and merge them after
        $stdoutLog = $LogFile
        $stderrLog = $LogFile -replace '\.log$', '_err.log'
        
        $process = Start-Process -FilePath $exePath -ArgumentList $Arguments `
            -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog `
            -NoNewWindow -PassThru -ErrorAction Stop
        
        return $process
    }
    catch {
        Write-Host "ERROR: Failed to start $Name server: $_" -ForegroundColor Red
        if ($Critical) {
            Write-Host "This is a critical server failure. Aborting tests." -ForegroundColor Red
            exit 1
        }
        return $null
    }
}

# Helper function to stop servers by name
function Stop-Servers {
    param(
        [string]$ProcessName,
        [switch]$IgnoreErrors = $true
    )
    
    try {
        Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction Stop
    }
    catch {
        if (-not $IgnoreErrors) {
            Write-Host "Warning: Error stopping $ProcessName : $_" -ForegroundColor Yellow
        }
    }
    Start-Sleep -Milliseconds 500
}

# ============================================================================
Write-Host ""
Write-Host "============================================================================"
Write-Host "Starting AB emulator for fast ControlLogix tests."
Write-Host "============================================================================"
Write-Host ""

$logixFast = Start-Server -Name "ControlLogix Fast" -Executable "ab_server.exe" `
    -Arguments @('--debug', '--plc=ControlLogix', '--path=1,0', 
                 '--tag=TestBigArray:DINT[2000]', '--tag=Test_Array_1:DINT[1000]',
                 '--tag=Test_Array_2x3:DINT[2,3]', '--tag=Test_Array_2x3x4:DINT[2,3,4]') `
    -LogFile "logix_fast_emulator.log"

Start-Sleep -Seconds 3

Write-Host "Checking that ab_server.exe is running..."
$serverProcess = Get-Process -Name "ab_server" -ErrorAction SilentlyContinue
if ($serverProcess) {
    $serverProcess | Format-Table -AutoSize
} else {
    Write-Host "WARNING: ab_server.exe not running. Skipping fast ControlLogix tests." -ForegroundColor Yellow
}

if ($serverProcess) {

# Test 1: basic large tag read/write
Run-Test -Name "basic large tag read/write" -Executable "tag_rw2.exe" `
    -Arguments @('--type=sint32',
                 '--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1000&name=TestBigArray',
                 '--debug=4', '--write=1,2,3,4,5,6,7,8,9') `
    -LogFile "${script:TestCount}_big_tag_test.log"

# Test 2: stress RC memory code
Run-Test -Name "stress RC memory code" -Executable "stress_rc_mem.exe" `
    -Arguments @() -LogFile "${script:TestCount}_stress_rc_mem_test.log"

# Test 3: CIP thread stress
Run-Test -Name "CIP thread stress" -Executable "thread_stress.exe" `
    -Arguments @() -LogFile "${script:TestCount}_thread_stress_test.log"

# Test 4: auto sync
Run-Test -Name "auto sync" -Executable "test_auto_sync.exe" `
    -Arguments @() -LogFile "${script:TestCount}_auto_sync_test.log"

# Test 5: indexed tags
Run-Test -Name "indexed tags" -Executable "test_indexed_tags.exe" `
    -Arguments @() -LogFile "${script:TestCount}_indexed_tags_test.log"

# Test 6: hard library shutdown
Run-Test -Name "hard library shutdown" -Executable "test_shutdown.exe" `
    -Arguments @() -LogFile "${script:TestCount}_shutdown_test.log"
}

Write-Host ""
Write-Host "Killing AB emulator."
Stop-Servers -ProcessName "ab_server"

# ============================================================================
Write-Host ""
Write-Host "============================================================================"
Write-Host "Starting stand-alone tests."
Write-Host "============================================================================"
Write-Host ""

# Test 7: Test async reconnect after PLC outage
Run-Test -Name "Test async reconnect after PLC outage" -Executable "test_reconnect_after_outage_async.exe" `
    -Arguments @((Join-Path $TestDir "ab_server.exe")) `
    -LogFile "${script:TestCount}_reconnect_async_test.log"

# Test 8: Test sync reconnect after PLC outage
Run-Test -Name "Test sync reconnect after PLC outage" -Executable "test_reconnect_after_outage_sync.exe" `
    -Arguments @((Join-Path $TestDir "ab_server.exe")) `
    -LogFile "${script:TestCount}_reconnect_sync_test.log"

# ============================================================================
Write-Host ""
Write-Host "============================================================================"
Write-Host "Starting AB emulator for functional/slow ControlLogix tests."
Write-Host "============================================================================"
Write-Host ""

$logixSlow = Start-Server -Name "ControlLogix Slow" -Executable "ab_server.exe" `
    -Arguments @('--plc=ControlLogix', '--path=1,0',
                 '--tag=TestBigArray:DINT[2000]', '--tag=Test_Array_1:DINT[1000]',
                 '--tag=Test_Array_2x3:DINT[2,3]', '--tag=Test_Array_2x3x4:DINT[2,3,4]',
                 '--delay=50') `
    -LogFile "logix_slow_emulator.log"

Start-Sleep -Seconds 1

if ($logixSlow -and -not $logixSlow.HasExited) {
    # Test 9: emulator test callbacks
    Run-Test -Name "emulator test callbacks" -Executable "test_callback.exe" `
        -Arguments @() -LogFile "${script:TestCount}_callback_test.log"
} else {
    Write-Host "WARNING: ControlLogix Slow server not running. Skipping tests." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Killing AB emulator."
Stop-Servers -ProcessName "ab_server"

# ============================================================================
Write-Host ""
Write-Host "============================================================================"
Write-Host "Starting AB emulator for Micro800 tests."
Write-Host "============================================================================"
Write-Host ""

$micro800 = Start-Server -Name "Micro800" -Executable "ab_server.exe" `
    -Arguments @('--debug', '--plc=Micro800', '--tag=TestDINTArray:DINT[10]') `
    -LogFile "micro800_emulator.log"

Start-Sleep -Seconds 1

if ($micro800 -and -not $micro800.HasExited) {
    # Test 10: Micro800 tag read/write
    Run-Test -Name "Micro800 tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=sint32',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&cpu=micro800&elem_size=4&elem_count=1&name=TestDINTArray[0]',
                     '--debug=2') `
        -LogFile "${script:TestCount}_micro800_test.log"
} else {
    Write-Host "WARNING: Micro800 server not running. Skipping tests." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Killing Micro800 emulator."
Stop-Servers -ProcessName "ab_server"

# ============================================================================
Write-Host ""
Write-Host "============================================================================"
Write-Host "Starting AB emulator for Omron tests."
Write-Host "============================================================================"
Write-Host ""

$omron = Start-Server -Name "Omron" -Executable "ab_server.exe" `
    -Arguments @('--debug', '--plc=Omron', '--tag=TestDINTArray:DINT[10]') `
    -LogFile "omron_emulator.log"

Start-Sleep -Seconds 1

if ($omron -and -not $omron.HasExited) {
    # Test 11: Omron NJ/NX tag read/write
    Run-Test -Name "Omron NJ/NX tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=sint32',
                     '--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray',
                     '--debug=2') `
        -LogFile "${script:TestCount}_omron_test.log"
} else {
    Write-Host "WARNING: Omron server not running. Skipping tests." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Killing Omron emulator."
Stop-Servers -ProcessName "ab_server"

# ============================================================================
Write-Host ""
Write-Host "============================================================================"
Write-Host "Starting AB emulator for Micrologix tests."
Write-Host "============================================================================"
Write-Host ""

$micrologix = Start-Server -Name "Micrologix" -Executable "ab_server.exe" `
    -Arguments @('--debug', '--plc=Micrologix', '--tag=B3[10]', '--tag=N7[10]', '--tag=L19[10]') `
    -LogFile "micrologix_emulator.log"

Start-Sleep -Seconds 1

if ($micrologix -and -not $micrologix.HasExited) {
    # Test 12: Micrologix B data file tag read/write
    Run-Test -Name "B data file Micrologix tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=sint16',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&plc=micrologix&elem_size=2&elem_count=10&name=B3:0',
                     '--debug=2') `
        -LogFile "${script:TestCount}_micrologix_b_test.log"

    # Test 13: Micrologix B bit data file tag read/write
    Run-Test -Name "B bit data file Micrologix tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=uint8',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&plc=micrologix&elem_size=1&elem_count=1&name=B3:0/1',
                     '--debug=2') `
        -LogFile "${script:TestCount}_micrologix_b_bit_test.log"

    # Test 14: Micrologix N data file tag read/write
    Run-Test -Name "N data file Micrologix tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=sint16',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&plc=micrologix&elem_size=2&elem_count=10&name=N7:0',
                     '--debug=2') `
        -LogFile "${script:TestCount}_micrologix_n_test.log"

    # Test 15: Micrologix N bit data file tag read/write
    Run-Test -Name "N bit data file Micrologix tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=uint8',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&plc=micrologix&elem_size=1&elem_count=1&name=N7:0/1',
                     '--debug=2') `
        -LogFile "${script:TestCount}_micrologix_n_bit_test.log"

    # Test 16: Micrologix L data file tag read/write
    Run-Test -Name "L data file Micrologix tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=sint32',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&plc=micrologix&elem_size=4&elem_count=10&name=L19:0',
                     '--debug=2') `
        -LogFile "${script:TestCount}_micrologix_l_test.log"

    # Test 17: Micrologix L bit data file tag read/write
    Run-Test -Name "L bit data file Micrologix tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=uint8',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&plc=micrologix&elem_size=1&elem_count=1&name=L19:0/1',
                     '--debug=2') `
        -LogFile "${script:TestCount}_micrologix_l_bit_test.log"
} else {
    Write-Host "WARNING: Micrologix server not running. Skipping tests." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Killing Micrologix emulator."
Stop-Servers -ProcessName "ab_server"

# ============================================================================
Write-Host ""
Write-Host "============================================================================"
Write-Host "Starting AB emulator for PLC5 tests."
Write-Host "============================================================================"
Write-Host ""

$plc5 = Start-Server -Name "PLC5" -Executable "ab_server.exe" `
    -Arguments @('--debug', '--plc=PLC/5', '--tag=B3[10]', '--tag=N7[10]') `
    -LogFile "plc5_emulator.log"

Start-Sleep -Seconds 1

if ($plc5 -and -not $plc5.HasExited) {
    # Test 18-24: PLC5 tests
    Run-Test -Name "B data file PLC5 tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=sint16',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&plc=plc5&elem_size=2&elem_count=10&name=B3:0',
                     '--debug=2') `
        -LogFile "${script:TestCount}_plc5_b_test.log"

    Run-Test -Name "B bit data file PLC5 tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=uint8',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&plc=plc5&elem_size=1&elem_count=1&name=B3:0/1',
                     '--debug=2') `
        -LogFile "${script:TestCount}_plc5_b_bit_test.log"

    Run-Test -Name "N data file PLC5 tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=sint16',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&plc=plc5&elem_size=2&elem_count=10&name=N7:0',
                     '--debug=2') `
        -LogFile "${script:TestCount}_plc5_n_test.log"

    Run-Test -Name "N bit data file PLC5 tag read/write" -Executable "tag_rw2.exe" `
        -Arguments @('--type=uint8',
                     '--tag=protocol=ab_eip&gateway=127.0.0.1&plc=plc5&elem_size=1&elem_count=1&name=N7:0/1',
                     '--debug=2') `
        -LogFile "${script:TestCount}_plc5_n_bit_test.log"
} else {
    Write-Host "WARNING: PLC5 server not running. Skipping tests." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Killing emulators."
Stop-Servers -ProcessName "ab_server"

# ============================================================================
Write-Host ""
Write-Host "============================================================================"
Write-Host "Starting Modbus server tests."
Write-Host "============================================================================"
Write-Host ""

$modbusServer = Start-Server -Name "Modbus" -Executable "modbus_server.exe" `
    -Arguments @('--listen=127.0.0.1:1502', '--listen=127.0.0.1:2502', '--debug=DETAIL') `
    -LogFile "modbus_server.log"

Start-Sleep -Seconds 2

# Check if Modbus server is running
$modbusProcess = Get-Process -Name "modbus_server" -ErrorAction SilentlyContinue
if (-not $modbusProcess) {
    Write-Host "ERROR: Modbus server failed to start or crashed" -ForegroundColor Red
    Write-Host "Skipping Modbus tests but continuing with remaining tests..." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Modbus server output:"
    if (Test-Path "modbus_server.log") {
        Get-Content "modbus_server.log"
    }
} else {
    Write-Host "Modbus server is running (PID: $($modbusProcess.Id))"

    # Test 25: Modbus multiple connections
    Run-Test -Name "Modbus multiple connections" -Executable "test_modbus_multiple.exe" `
        -Arguments @() -LogFile "${script:TestCount}_modbus_multiple_test.log"

    Write-Host ""
    Write-Host "Killing Modbus server."
    Stop-Servers -ProcessName "modbus_server"
}

Start-Sleep -Seconds 2

# ============================================================================
Write-Host ""
Write-Host "============================================================================"
Write-Host "Results:"
Write-Host "============================================================================"
Write-Host ""
Write-Host "$script:TestCount tests."
Write-Host "$script:Successes successes." -ForegroundColor Green
Write-Host "$script:Failures failures." -ForegroundColor $(if ($script:Failures -eq 0) { 'Green' } else { 'Red' })
Write-Host ""

if ($script:Failures -eq 0) {
    exit 0
} else {
    exit 1
}
