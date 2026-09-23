#!/bin/bash

# SlickLogger Benchmark Runner Script
# Usage: ./run_benchmarks.sh [quick|full|custom]

set -e  # Exit on any error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

# Default benchmark parameters
DEFAULT_ITERATIONS=50000
DEFAULT_RUNS=3
QUICK_ITERATIONS=10000
QUICK_RUNS=2

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}"
}

print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if build directory exists
check_build() {
    if [[ ! -d "$BUILD_DIR" ]]; then
        print_error "Build directory not found. Please run cmake first:"
        echo "  mkdir build && cd build"
        echo "  cmake -DBUILD_SLICK_LOGGER_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release .."
        echo "  cmake --build . --config Release"
        exit 1
    fi
    
    if [[ ! -f "$BUILD_DIR/benchmarks/slick_logger_benchmark" && ! -f "$BUILD_DIR/benchmarks/Release/slick_logger_benchmark.exe" ]]; then
        print_error "Benchmark executables not found. Please build with:"
        echo "  cmake --build . --config Release"
        exit 1
    fi
}

# Detect benchmark executable names (Linux vs Windows)
detect_executables() {
    if [[ -f "$BUILD_DIR/benchmarks/slick_logger_benchmark" ]]; then
        MAIN_BENCHMARK="$BUILD_DIR/benchmarks/slick_logger_benchmark"
        LATENCY_BENCHMARK="$BUILD_DIR/benchmarks/latency_benchmark"
        THROUGHPUT_BENCHMARK="$BUILD_DIR/benchmarks/throughput_benchmark"
        MEMORY_BENCHMARK="$BUILD_DIR/benchmarks/memory_benchmark"
    elif [[ -f "$BUILD_DIR/benchmarks/Release/slick_logger_benchmark.exe" ]]; then
        MAIN_BENCHMARK="$BUILD_DIR/benchmarks/Release/slick_logger_benchmark.exe"
        LATENCY_BENCHMARK="$BUILD_DIR/benchmarks/Release/latency_benchmark.exe"
        THROUGHPUT_BENCHMARK="$BUILD_DIR/benchmarks/Release/throughput_benchmark.exe"
        MEMORY_BENCHMARK="$BUILD_DIR/benchmarks/Release/memory_benchmark.exe"
    else
        print_error "Could not find benchmark executables"
        exit 1
    fi
}

# System information
print_system_info() {
    print_header "SYSTEM INFORMATION"
    echo "Date: $(date)"
    echo "Hostname: $(hostname)"
    echo "OS: $(uname -a)"
    
    if command -v nproc &> /dev/null; then
        echo "CPU cores: $(nproc)"
    fi
    
    if command -v free &> /dev/null; then
        echo "Memory: $(free -h | grep '^Mem:' | awk '{print $2}')"
    fi
    
    if [[ -f /proc/cpuinfo ]]; then
        echo "CPU model: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
    fi
    echo
}

# Run quick benchmarks  
run_quick() {
    print_header "QUICK BENCHMARK SUITE"
    print_info "Running with reduced iterations for faster results..."
    print_info "Iterations: $QUICK_ITERATIONS, Runs: $QUICK_RUNS"
    echo
    
    print_info "Running main benchmark suite..."
    "$MAIN_BENCHMARK" $QUICK_ITERATIONS $QUICK_RUNS
    
    echo
    print_info "Quick benchmark completed!"
}

# Run full benchmarks
run_full() {
    print_header "FULL BENCHMARK SUITE"
    print_info "Running comprehensive benchmarks - this may take several minutes..."
    print_info "Iterations: $DEFAULT_ITERATIONS, Runs: $DEFAULT_RUNS"
    echo
    
    print_info "Running main benchmark suite..."
    "$MAIN_BENCHMARK" $DEFAULT_ITERATIONS $DEFAULT_RUNS
    
    echo
    print_info "Running detailed latency analysis..."
    "$LATENCY_BENCHMARK"
    
    echo  
    print_info "Running throughput scaling analysis..."
    "$THROUGHPUT_BENCHMARK"
    
    echo
    print_info "Running memory usage analysis..."
    "$MEMORY_BENCHMARK"
    
    echo
    print_info "Full benchmark suite completed!"
}

# Run custom benchmarks
run_custom() {
    print_header "CUSTOM BENCHMARK CONFIGURATION"
    
    echo "Select benchmark type:"
    echo "1) Main suite only"
    echo "2) Latency analysis only"  
    echo "3) Throughput analysis only"
    echo "4) Memory analysis only"
    echo "5) All benchmarks"
    
    read -p "Enter choice (1-5): " choice
    
    case $choice in
        1)
            echo "Enter iterations (default: $DEFAULT_ITERATIONS):"
            read -p "> " iterations
            iterations=${iterations:-$DEFAULT_ITERATIONS}
            
            echo "Enter number of runs (default: $DEFAULT_RUNS):"
            read -p "> " runs
            runs=${runs:-$DEFAULT_RUNS}
            
            print_info "Running main benchmark with $iterations iterations, $runs runs..."
            "$MAIN_BENCHMARK" $iterations $runs
            ;;
        2)
            print_info "Running latency analysis..."
            "$LATENCY_BENCHMARK"
            ;;
        3)
            print_info "Running throughput analysis..."
            "$THROUGHPUT_BENCHMARK"
            ;;
        4)
            print_info "Running memory analysis..."
            "$MEMORY_BENCHMARK"
            ;;
        5)
            run_full
            ;;
        *)
            print_error "Invalid choice"
            exit 1
            ;;
    esac
}

# Setup output directory
setup_output() {
    mkdir -p "$BUILD_DIR/benchmark_results"
    cd "$BUILD_DIR/benchmark_results"
    print_info "Output directory: $(pwd)"
    print_info "Log files will be created in: $(pwd)/benchmark_logs/"
    echo
}

# Performance tuning suggestions
suggest_tuning() {
    print_header "PERFORMANCE TUNING SUGGESTIONS"
    
    print_info "For more consistent results, consider:"
    echo "  • Close unnecessary applications"
    echo "  • Run with sudo for process priority:"
    echo "    sudo nice -n -10 $0 $1"
    echo "  • Disable CPU frequency scaling (Linux):"
    echo "    echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
    echo "  • Set CPU affinity:"
    echo "    taskset -c 0-3 $0 $1"
    echo
}

# Main function
main() {
    local mode=${1:-full}
    
    print_header "SLICKLOGGER BENCHMARK SUITE"
    
    # Check prerequisites
    check_build
    detect_executables
    setup_output
    print_system_info
    suggest_tuning "$mode"
    
    # Run appropriate benchmark mode
    case $mode in
        quick)
            run_quick
            ;;
        full)
            run_full
            ;;
        custom)
            run_custom
            ;;
        *)
            print_error "Unknown mode: $mode"
            echo "Usage: $0 [quick|full|custom]"
            echo "  quick  - Fast benchmark with reduced iterations"
            echo "  full   - Complete benchmark suite (default)"
            echo "  custom - Interactive benchmark selection"
            exit 1
            ;;
    esac
    
    echo
    print_header "BENCHMARK RESULTS"
    print_info "Results saved in: $(pwd)"
    print_info "Log files available in: $(pwd)/benchmark_logs/"
    
    if [[ -d "benchmark_logs" ]]; then
        local log_count=$(find benchmark_logs -name "*.log" | wc -l)
        print_info "Generated $log_count log files"
    fi
    
    echo
    print_info "To analyze results:"
    echo "  • View individual log files for detailed output"
    echo "  • Compare throughput numbers across libraries"
    echo "  • Check latency percentiles for responsiveness"
    echo "  • Monitor memory usage for efficiency"
}

# Handle Ctrl+C gracefully
cleanup() {
    print_warning "Benchmark interrupted by user"
    exit 130
}

trap cleanup SIGINT

# Run main function with all arguments
main "$@"