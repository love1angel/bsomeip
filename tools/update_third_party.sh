#!/usr/bin/env bash
set -euo pipefail

# Update all git submodules under third_party to latest remote commits.
# - Initializes missing submodules
# - Syncs URLs from .gitmodules
# - Updates each submodule to its remote default branch tip
# - Recurses into nested submodules

ROOT_DIR="$(git rev-parse --show-toplevel)"
cd "$ROOT_DIR"

if [[ ! -f .gitmodules ]]; then
  echo "No .gitmodules found in $ROOT_DIR"
  exit 0
fi

echo "[1/4] Syncing submodule URLs"
git submodule sync --recursive

echo "[2/4] Initializing submodules"
git submodule update --init --recursive

echo "[3/4] Updating submodules to latest remote commits"
git submodule foreach --recursive '
  set -euo pipefail

  if [[ -n "$(git status --porcelain)" ]]; then
    echo "[skip] $name ($sm_path): working tree is dirty"
    exit 0
  fi

  git fetch --tags origin >/dev/null 2>&1 || {
    echo "[warn] $name ($sm_path): fetch failed"
    exit 0
  }

  default_ref="$(git symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>/dev/null || true)"
  default_branch="${default_ref#origin/}"

  if [[ -z "$default_branch" ]]; then
    if git show-ref --verify --quiet refs/remotes/origin/main; then
      default_branch="main"
    elif git show-ref --verify --quiet refs/remotes/origin/master; then
      default_branch="master"
    else
      echo "[warn] $name ($sm_path): no default branch detected"
      exit 0
    fi
  fi

  if ! git show-ref --verify --quiet "refs/remotes/origin/$default_branch"; then
    echo "[warn] $name ($sm_path): origin/$default_branch not found"
    exit 0
  fi

  git checkout -q "origin/$default_branch"
  echo "[ok]   $name ($sm_path) -> origin/$default_branch @ $(git rev-parse --short HEAD)"
'

echo "[4/4] Summary"
git status --short

echo
echo "Done. Review submodule pointer changes, then commit if needed."
