# Contributing to yafl

## Getting Started

1. Fork the repository
2. Clone your fork locally
3. Create a feature branch from `prerelease`

## Making Changes

1. Make your changes and test them locally
2. For coverage testing, run: `./scripts/generate_coverage.sh`
3. Commit with clear, descriptive messages

## Submitting Changes

**Important:** Always open pull requests against the **`prerelease`** branch, not `release`.

- The `prerelease` branch is for active development
- The `release` branch is only for tagged releases
- PRs to `prerelease` will trigger automated tests and coverage analysis

## Branch Workflow

- **prerelease**: Development branch (your PRs go here)
- **release**: Tagged releases (automatically updated from prerelease)

## Code Style

- C11 standard
- Use existing code style as a guide
- Include comments for complex logic

## Testing

All tests must pass before PRs are merged:

```bash
mkdir -p build && cd build
cmake -B . -DTARGET=<your-triple> ..
ctest --output-on-failure
```

## Questions?

Open an issue for discussion before making major changes.
