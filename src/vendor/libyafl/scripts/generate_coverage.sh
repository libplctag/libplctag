#!/usr/bin/env bash
set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo -e "${YELLOW}=== YAFL Code Coverage Generator ===${NC}"
echo

# Check for lcov
if ! command -v lcov &> /dev/null; then
    echo -e "${RED}Error: lcov is not installed${NC}"
    echo "Install it with:"
    echo "  macOS: brew install lcov"
    echo "  Linux: sudo apt-get install lcov"
    exit 1
fi

# Check for cmake
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}Error: cmake is not installed${NC}"
    exit 1
fi

# Check for genhtml (comes with lcov)
if ! command -v genhtml &> /dev/null; then
    echo -e "${RED}Error: genhtml is not installed${NC}"
    exit 1
fi

cd "$PROJECT_DIR"

echo -e "${GREEN}Step 1: Configuring CMake with coverage enabled...${NC}"
cmake -S . -B build -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
echo

echo -e "${GREEN}Step 2: Building project...${NC}"
cmake --build build
echo

echo -e "${GREEN}Step 3: Running tests...${NC}"
ctest --test-dir build --output-on-failure
echo

echo -e "${GREEN}Step 4: Capturing coverage data...${NC}"
lcov --capture --directory build --output-file coverage.info
echo

echo -e "${GREEN}Step 5: Filtering system files from coverage...${NC}"
lcov --remove coverage.info '/usr/*' --output-file coverage.info --ignore-errors unused
echo

echo -e "${GREEN}Step 6: Generating HTML report...${NC}"
genhtml coverage.info --output-directory docs/coverage --title "yafl Code Coverage"
echo

echo -e "${GREEN}Step 7: Generating coverage badge...${NC}"
"$SCRIPT_DIR/make_coverage_badge.sh" docs/coverage.svg coverage.info
echo

echo -e "${GREEN}=== Coverage Generation Complete ===${NC}"
echo
echo -e "Coverage report: ${YELLOW}docs/coverage/index.html${NC}"
echo -e "Coverage badge: ${YELLOW}docs/coverage.svg${NC}"
echo
echo "To view the report, open:"
echo "  file://$(cd "$PROJECT_DIR" && pwd)/docs/coverage/index.html"
echo
echo -e "${YELLOW}Note:${NC} Clean the build directory with:"
echo "  rm -rf build coverage.info docs/coverage docs/coverage.svg"
