# Change intents

Add one Markdown file per releasable change:

```markdown
---
'posthog-unreal': patch
---

Describe the user-visible change.
```

Use `patch`, `minor`, or `major`. `posthog-unreal` covers the whole SDK: C++, Blueprint APIs, the Rust bridge, and packaging. The Release workflow applies pending intents on `main`, synchronizes the plugin and bridge versions, and publishes after CI and approval. pnpm records consumed intents in `ledger.yaml`; do not delete or edit that ledger manually.

See [RELEASING](../RELEASING.md) for the full process.
