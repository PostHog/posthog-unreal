# Contributing to PostHog Unreal SDK

Thank you for your interest in contributing!

Read our [AI contributions policy](AI_POLICY.md) before submitting AI-assisted
contributions.

## Getting started

```bash
git clone git@github.com:PostHog/posthog-unreal.git
cd posthog-unreal
```

The SDK is in early development. Engine requirements, supported platforms, build
instructions, and test commands will be documented as the implementation lands.

For substantial changes, open an issue first to discuss the approach.

## Development guidelines

- Follow Unreal Engine C++ conventions.
- Document public APIs and update user-facing documentation when behavior changes.
- Add tests for new functionality and bug fixes.
- Keep changes focused; avoid unrelated cleanup.
- Keep credentials and local configuration out of source control.

## Pull requests

1. Create a branch from `main` with a descriptive name.
2. Implement the change and add relevant tests.
3. Run the checks available for the affected code and explain how you tested it.
4. Describe the motivation and link any related issues.
5. Call out breaking changes and their migration path.

Use a [Conventional Commits](https://www.conventionalcommits.org/) pull request
title, such as `feat: add event capture` or `fix: preserve queued events`.

## Getting help

Check existing [issues](https://github.com/PostHog/posthog-unreal/issues) before
opening a new one. For general questions, use the
[PostHog community](https://posthog.com/questions).

## License

By contributing, you agree that your contributions will be licensed under the
[MIT License](LICENSE).
