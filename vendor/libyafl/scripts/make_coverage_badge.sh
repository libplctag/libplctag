#!/usr/bin/env bash
set -e

# Usage: make_coverage_badge.sh <output_svg_path> [coverage_info_path]
OUTPUT_SVG="${1:-coverage.svg}"
COVERAGE_INFO="${2:-coverage.info}"

# Check if coverage.info exists
if [ ! -f "$COVERAGE_INFO" ]; then
    echo "Error: Coverage file not found: $COVERAGE_INFO"
    exit 1
fi

# Extract coverage percentage
COVERAGE=$(lcov --summary "$COVERAGE_INFO" | grep "lines" | awk '{print $2}' | sed 's/%//')
if [ -z "$COVERAGE" ]; then
    echo "Error: Could not extract coverage from $COVERAGE_INFO"
    exit 1
fi

COVERAGE_INT=${COVERAGE%.*}

# Determine color based on coverage percentage
if [ "$COVERAGE_INT" -ge 90 ]; then
  COLOR="brightgreen"
elif [ "$COVERAGE_INT" -ge 75 ]; then
  COLOR="yellow"
else
  COLOR="red"
fi

# Ensure output directory exists
OUTPUT_DIR=$(dirname "$OUTPUT_SVG")
if [ "$OUTPUT_DIR" != "." ]; then
  mkdir -p "$OUTPUT_DIR"
fi

# Generate SVG badge
cat > "$OUTPUT_SVG" <<EOF
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="20">
  <linearGradient id="b" x2="0" y2="100%">
    <stop offset="0" stop-color="#bbb" stop-opacity=".1"/>
    <stop offset="1" stop-opacity=".1"/>
  </linearGradient>
  <mask id="a">
    <rect width="120" height="20" rx="3" fill="#fff"/>
  </mask>
  <g mask="url(#a)">
    <rect width="70" height="20" fill="#555"/>
    <rect x="70" width="50" height="20" fill="${COLOR}"/>
    <rect width="120" height="20" fill="url(#b)"/>
  </g>
  <g fill="#fff" text-anchor="middle"
     font-family="DejaVu Sans,Verdana,Geneva,sans-serif"
     font-size="11">
    <text x="35" y="14">coverage</text>
    <text x="95" y="14">${COVERAGE}%</text>
  </g>
</svg>
EOF

echo "Generated coverage badge: $OUTPUT_SVG ($COVERAGE%)"
