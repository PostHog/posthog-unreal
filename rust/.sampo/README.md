# Release changesets

Run Sampo from `rust/`. Add a Markdown file in `changesets/` for each user-visible change:

```md
---
cargo/posthog-unreal-ffi: patch
---

Describe the user-visible change.
```

Use patch for fixes, minor for features and major for breaking changes.
See [the release guide](../../docs/releasing.md) for preparation and publication.
