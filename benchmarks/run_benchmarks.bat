@echo off
setlocal enabledelayedexpansion

:: SlickLogger Benchmark Runner Script for Windows
:: Usage: run_benchmarks.bat [quick|full|custom]

set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%SCRIPT_DIR%.."
set "BUILD_DIR=%PROJECT_ROOT%\build"

:: Default benchmark parameters
set DEFAULT_ITERATIONS=50000
set DEFAULT_RUNS=3
set QUICK_ITERATIONS=10000
set QUICK_RUNS=2

:: Set mode (default to full)
set "MODE=%1"
if "%MODE%"=="" set "MODE=full"

echo ========================================
echo SLICKLOGGER BENCHMARK SUITE
echo ========================================
echo.

:: Check if build directory exists
if not exist "%BUILD_DIR%" (
    echo [ERROR] Build directory not found. Please run cmake first:
    echo   mkdir build ^&^& cd build
    echo   cmake -DBUILD_SLICK_LOGGER_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release ..
    echo   cmake --build . --config Release
    exit /b 1
)

:: Detect benchmark executables
if exist "%BUILD_DIR%\benchmarks\Release\slick_logger_benchmark.exe" (
    set "MAIN_BENCHMARK=%BUILD_DIR%\benchmarks\Release\slick_logger_benchmark.exe"
    set "LATENCY_BENCHMARK=%BUILD_DIR%\benchmarks\Release\latency_benchmark.exe"
    set "THROUGHPUT_BENCHMARK=%BUILD_DIR%\benchmarks\Release\throughput_benchmark.exe"
    set "MEMORY_BENCHMARK=%BUILD_DIR%\benchmarks\Release\memory_benchmark.exe"
) else if exist "%BUILD_DIR%\benchmarks\slick_logger_benchmark.exe" (
    set "MAIN_BENCHMARK=%BUILD_DIR%\benchmarks\slick_logger_benchmark.exe"
    set "LATENCY_BENCHMARK=%BUILD_DIR%\benchmarks\latency_benchmark.exe"
    set "THROUGHPUT_BENCHMARK=%BUILD_DIR%\benchmarks\throughput_benchmark.exe"
    set "MEMORY_BENCHMARK=%BUILD_DIR%\benchmarks\memory_benchmark.exe"
) else (
    echo [ERROR] Benchmark executables not found. Please build with:
    echo   cmake --build . --config Release
    exit /b 1
)

:: Setup output directory
if not exist "%BUILD_DIR%\benchmark_results" mkdir "%BUILD_DIR%\benchmark_results"
cd /d "%BUILD_DIR%\benchmark_results"

:: Print system information
echo ========================================
echo SYSTEM INFORMATION  
echo ========================================
echo Date: %DATE% %TIME%
echo Computer: %COMPUTERNAME%
echo OS: %OS%
echo Processor: %PROCESSOR_IDENTIFIER%
echo Cores: %NUMBER_OF_PROCESSORS%
echo.

:: Performance tuning suggestions
echo ========================================
echo PERFORMANCE TUNING SUGGESTIONS
echo ========================================
echo For more consistent results, consider:
echo   • Close unnecessary applications
echo   • Run as Administrator for higher priority
echo   • Set process affinity in Task Manager
echo   • Disable Windows Update during benchmarking
echo.

:: Run appropriate benchmark mode
if "%MODE%"=="quick" goto :run_quick
if "%MODE%"=="full" goto :run_full  
if "%MODE%"=="custom" goto :run_custom

echo [ERROR] Unknown mode: %MODE%
echo Usage: %0 [quick^|full^|custom]
echo   quick  - Fast benchmark with reduced iterations
echo   full   - Complete benchmark suite (default)
echo   custom - Interactive benchmark selection
exit /b 1

:run_quick
echo ========================================
echo QUICK BENCHMARK SUITE
echo ========================================
echo [INFO] Running with reduced iterations for faster results...
echo [INFO] Iterations: %QUICK_ITERATIONS%, Runs: %QUICK_RUNS%
echo.

echo [INFO] Running main benchmark suite...
"%MAIN_BENCHMARK%" %QUICK_ITERATIONS% %QUICK_RUNS%
if errorlevel 1 (
    echo [ERROR] Main benchmark failed
    exit /b 1
)

echo.
echo [INFO] Quick benchmark completed!
goto :end

:run_full
echo ========================================
echo FULL BENCHMARK SUITE
echo ========================================
echo [INFO] Running comprehensive benchmarks - this may take several minutes...
echo [INFO] Iterations: %DEFAULT_ITERATIONS%, Runs: %DEFAULT_RUNS%
echo.

echo [INFO] Running main benchmark suite...
"%MAIN_BENCHMARK%" %DEFAULT_ITERATIONS% %DEFAULT_RUNS%
if errorlevel 1 (
    echo [ERROR] Main benchmark failed
    exit /b 1
)

echo.
echo [INFO] Running detailed latency analysis...
"%LATENCY_BENCHMARK%"
if errorlevel 1 (
    echo [ERROR] Latency benchmark failed
    exit /b 1
)

echo.
echo [INFO] Running throughput scaling analysis...
"%THROUGHPUT_BENCHMARK%"
if errorlevel 1 (
    echo [ERROR] Throughput benchmark failed
    exit /b 1
)

echo.
echo [INFO] Running memory usage analysis...
"%MEMORY_BENCHMARK%"
if errorlevel 1 (
    echo [ERROR] Memory benchmark failed
    exit /b 1
)

echo.
echo [INFO] Full benchmark suite completed!
goto :end

:run_custom
echo ========================================
echo CUSTOM BENCHMARK CONFIGURATION
echo ========================================
echo Select benchmark type:
echo 1) Main suite only
echo 2) Latency analysis only
echo 3) Throughput analysis only
echo 4) Memory analysis only
echo 5) All benchmarks
echo.
set /p choice=Enter choice (1-5): 

if "%choice%"=="1" (
    set /p iterations=Enter iterations (default: %DEFAULT_ITERATIONS%): 
    if "!iterations!"=="" set iterations=%DEFAULT_ITERATIONS%
    set /p runs=Enter number of runs (default: %DEFAULT_RUNS%): 
    if "!runs!"=="" set runs=%DEFAULT_RUNS%
    
    echo [INFO] Running main benchmark with !iterations! iterations, !runs! runs...
    "%MAIN_BENCHMARK%" !iterations! !runs!
) else if "%choice%"=="2" (
    echo [INFO] Running latency analysis...
    "%LATENCY_BENCHMARK%"
) else if "%choice%"=="3" (
    echo [INFO] Running throughput analysis...
    "%THROUGHPUT_BENCHMARK%"
) else if "%choice%"=="4" (
    echo [INFO] Running memory analysis...
    "%MEMORY_BENCHMARK%"
) else if "%choice%"=="5" (
    goto :run_full
) else (
    echo [ERROR] Invalid choice
    exit /b 1
)

:end
echo.
echo ========================================
echo BENCHMARK RESULTS
echo ========================================
echo [INFO] Results saved in: %CD%
echo [INFO] Log files available in: %CD%\benchmark_logs\

if exist "benchmark_logs" (
    for /f %%i in ('dir /b benchmark_logs\*.log 2^>nul ^| find /c /v ""') do (
        echo [INFO] Generated %%i log files
    )
)

echo.
echo [INFO] To analyze results:
echo   • View individual log files for detailed output
echo   • Compare throughput numbers across libraries  
echo   • Check latency percentiles for responsiveness
echo   • Monitor memory usage for efficiency

pause