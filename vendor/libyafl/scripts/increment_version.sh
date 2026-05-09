#!/bin/bash
set -e

VERSION_FILE="VERSION"
BUMP_TYPE="${1:-patch}"  # patch, minor, or major (default: patch)

if [ ! -f "$VERSION_FILE" ]; then
    echo "Error: $VERSION_FILE not found."
    exit 1
fi

CURRENT_VERSION=$(cat "$VERSION_FILE" | xargs)  # Strip whitespace

# Validate format (major.minor.patch)
if ! [[ $CURRENT_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: VERSION must be in format major.minor.patch, got: $CURRENT_VERSION"
    exit 1
fi

# Split version into components
IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT_VERSION"

# Increment based on bump type
case "$BUMP_TYPE" in
    major)
        MAJOR=$((MAJOR + 1))
        MINOR=0
        PATCH=0
        ;;
    minor)
        MINOR=$((MINOR + 1))
        PATCH=0
        ;;
    patch)
        PATCH=$((PATCH + 1))
        ;;
    *)
        echo "Error: BUMP_TYPE must be 'major', 'minor', or 'patch', got: $BUMP_TYPE"
        exit 1
        ;;
esac

NEW_VERSION="${MAJOR}.${MINOR}.${PATCH}"

echo "$NEW_VERSION" > "$VERSION_FILE"

echo "Version bumped: $CURRENT_VERSION -> $NEW_VERSION (type: $BUMP_TYPE)"
if [ -n "$GITHUB_OUTPUT" ]; then
    echo "version=$NEW_VERSION" >> "$GITHUB_OUTPUT"
fi