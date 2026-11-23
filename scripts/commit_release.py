#!/usr/bin/env python3
"""Create a signed release commit using GitHub's expected-parent check."""
import base64
import json
import os
from pathlib import Path
import subprocess

import release

# Only files written by pnpm and bump_version.py may change in a version commit.
ALLOWED = {
    "CHANGELOG.md",
    "package.json",
    ".changeset/ledger.yaml",
    "PostHog.uplugin",
    f"rust/{release.BRIDGE}/Cargo.toml",
    "rust/Cargo.lock",
}


def release_message(version):
    return f"chore: release {version} [version bump]"


def staged_changes(root, expected_tree):
    tree = subprocess.check_output(["git", "write-tree"], cwd=root, text=True).strip()
    if tree != expected_tree:
        raise ValueError("Staged tree does not match the verified release candidate")
    subprocess.run(["git", "diff", "--exit-code"], cwd=root, check=True)
    paths = subprocess.check_output(["git", "diff", "--cached", "--name-only", "-z"], cwd=root).decode().split("\0")
    additions, deletions = [], []
    for path in filter(None, paths):
        file = root / path
        consumed_intent = Path(path).parent == Path(".changeset") and file.suffix == ".md" and not file.exists()
        if path not in ALLOWED and not consumed_intent:
            raise ValueError(f"Unexpected release change: {path}")
        if file.exists():
            additions.append({"path": path, "contents": base64.b64encode(file.read_bytes()).decode()})
        else:
            deletions.append({"path": path})
    return additions, deletions


def github(*args, payload=None):
    result = subprocess.run(["gh", "api", *args], input=payload, text=True, stdout=subprocess.PIPE, check=True)
    return json.loads(result.stdout)


def create_release_commit(root, repository, version, expected_tree):
    additions, deletions = staged_changes(root, expected_tree)
    source = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    message = release_message(version)
    remote = github(f"repos/{repository}/commits/main")
    if remote["sha"] == source:
        request = {
            "query": "mutation($input: CreateCommitOnBranchInput!) { createCommitOnBranch(input: $input) { commit { oid } } }",
            "variables": {"input": {
                "branch": {"repositoryNameWithOwner": repository, "branchName": "main"},
                # GitHub rejects the commit if main moved after the candidate was prepared.
                "expectedHeadOid": source,
                "message": {"headline": message},
                "fileChanges": {"additions": additions, "deletions": deletions},
            }},
        }
        response = github("graphql", "--input", "-", payload=json.dumps(request))
        if response.get("errors"):
            raise RuntimeError(response["errors"])
        return response["data"]["createCommitOnBranch"]["commit"]["oid"]
    # A rerun after a later failure reuses its own signed commit, never anyone else's.
    if ([parent["sha"] for parent in remote["parents"]] == [source]
            and remote["commit"]["tree"]["sha"] == expected_tree
            and remote["commit"]["message"] == message
            and remote["commit"]["verification"]["verified"]):
        return remote["sha"]
    raise ValueError("main moved since candidate preparation; prepare a new candidate")


def main():
    sha = create_release_commit(release.ROOT, os.environ["GH_REPO"], os.environ["NEW_VERSION"],
                                os.environ["EXPECTED_TREE_SHA"])
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write(f"sha={sha}\n")
    print(f"Release commit {sha}")


if __name__ == "__main__":
    main()
