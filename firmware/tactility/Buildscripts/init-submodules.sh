#!/usr/bin/env bash
#
# Initialises the submodules this project builds against. Not a plain recursive update: ESP-ADF
# declares a whole ESP-IDF checkout and an esp-sr model pack, neither of which is used here.

set -euo pipefail

# The repository root, since that is where .gitmodules lives and what submodule paths are
# relative to; this script sits two levels down inside firmware/tactility.
cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"

ADF_PATH_IN_REPO="firmware/tactility/Libraries/esp-adf"

mapfile -t other_submodules < <(
    git config -f .gitmodules --get-regexp '\.path$' | awk '{print $2}' | grep -v "^${ADF_PATH_IN_REPO}$"
)

git submodule update --init --recursive -- "${other_submodules[@]}"

git submodule update --init -- "$ADF_PATH_IN_REPO"
git -C "$ADF_PATH_IN_REPO" submodule update --init components/esp-adf-libs
