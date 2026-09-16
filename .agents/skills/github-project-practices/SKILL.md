---
name: github-project-practices
description: Prepare, commit, push, review, and merge changes for the IoT-Trashcan repository with a fast GitHub Flow and evidence-based checks. Use only while working in this repository.
---

# GitHub Practices for IoT-Trashcan

Apply this workflow only when the active repository root is `IoT-Trashcan`. Stop if the repository identity or requested target is ambiguous.

## Preserve repository state

Before changing files:

1. Run `git status --short --branch`.
2. Read the relevant documentation and repository instructions.
3. Preserve unrelated or uncommitted work.
4. Confirm that the intended change has one clear outcome.

Never commit credentials, `.env` files, private keys, tokens, local virtual environments, dependencies, caches, logs, or build output. Do not rewrite shared history or force-push `main`.

## Branch workflow

Start from an updated `main` unless existing work already belongs to a suitable branch.

```bash
git switch main
git pull --ff-only
git switch -c <type>/<short-topic>
```

Use `docs/`, `feat/`, `fix/`, `refactor/`, `test/`, or `chore/` as the branch prefix. Keep branches short-lived and limited to one reviewable outcome.

Direct commits to `main` are limited to the initial repository bootstrap or an explicit emergency instruction. Normal work uses a pull request.

## Make focused commits

Inspect changes before staging:

```bash
git diff
git status --short
```

Stage explicit paths instead of `git add .`. Use a concise imperative commit subject with an optional Conventional Commit prefix:

```text
docs: correct architecture references
feat: add device telemetry endpoint
fix: reject invalid sensor readings
```

Split unrelated changes into separate commits or branches.

## Verify before push

Run the smallest relevant checks first, followed by the broadest checks supported by the current project. Derive commands from repository manifests and documentation. Do not invent a passing result when no automated check exists.

Minimum checks for every change:

```bash
git diff --check
git status --short
git diff --cached
```

Also verify:

- Markdown links and referenced files for documentation changes.
- Tests, lint, type checking, and build for affected application code.
- Firmware compile checks for affected embedded code.
- Secret and large-file checks before the first push or after adding binary files.

Record the exact commands and material results in the pull request. Classify unavailable checks as unverified and state the reason.

## Push and open a pull request

```bash
git push -u origin HEAD
gh pr create --fill
```

Keep the pull request small enough to review in one pass. Complete `.github/pull_request_template.md`, inspect the GitHub diff, and resolve all relevant review comments. Update the branch before merge when `main` changed materially.

## Merge and clean up

Merge only when checks pass and the pull request accurately describes the final diff. Prefer squash merge to keep `main` linear:

```bash
gh pr merge --squash --delete-branch
git switch main
git pull --ff-only
```

Do not merge with unresolved failures. Report a blocker with command output instead of bypassing a required check.

## Repository settings

Keep the GitHub repository private unless an explicit instruction changes visibility. Allow squash merge, delete merged branches automatically, block force pushes and deletion of `main`, and require pull requests when the account plan supports private-repository branch protection. Add required status checks only after stable CI jobs exist.
