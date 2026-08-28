#!/bin/bash
#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
# Copyright (c) 2026 Michael Vandeberg
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

set -e

if [ -z "$1" ]; then
    echo "No playbook supplied, using default playbook"
    PLAYBOOK="local-playbook.yml"
else
    PLAYBOOK="$1"
fi

echo "Building documentation with Antora..."
echo "Installing npm dependencies..."
npm ci

# The reference examples are injected by addons/extensions/reference-snippets.lua.
# MrDocs loads extensions only from <install>/share/mrdocs/addons/extensions, and
# its `addons-supplemental` config key is recognised but has no effect, so the
# extension has to be placed inside a MrDocs install. Doing it here rather than in
# CI means every caller gets it: this repository's docs workflow, the C++ Alliance
# doc build, and a plain local run. Without it the reference builds with no
# examples at all and reports success.
#
# develop-release rather than a tagged release: MrDocs' extension API postdates
# v0.8.0. The tag is rolling, so this URL always names the current develop build
# and needs no API call or token.
if [ -z "${MRDOCS_ROOT:-}" ]; then
  case "$(uname -s)" in
    Linux)  mrdocs_asset="MrDocs-develop-Linux.tar.gz" ;;
    Darwin) mrdocs_asset="MrDocs-develop-Darwin.tar.gz" ;;
    *) echo "No MrDocs build for $(uname -s); set MRDOCS_ROOT to an install." >&2
       exit 1 ;;
  esac
  mrdocs_dir="$(pwd)/build/mrdocs"
  # A cached install counts only if both halves are there. An interrupted tar can
  # leave bin/mrdocs behind without share/mrdocs, and testing for the binary alone
  # would take that half-install for a good one and skip the refetch. Extraction
  # therefore starts from an empty directory, so a partial extract can never be
  # mistaken for a complete one.
  #
  # Nothing invalidates this cache on age, and develop-release is a rolling tag,
  # so a long-lived tree keeps whichever build it first downloaded and two
  # developers can be on different ones. Refetching every build would cost a
  # download per run; `rm -rf doc/build/mrdocs` forces a fresh one.
  if ! { find "$mrdocs_dir" -type f -name mrdocs -perm -u+x 2>/dev/null | grep -q . &&
         find "$mrdocs_dir" -type d -path '*/share/mrdocs' 2>/dev/null | grep -q .; }; then
    echo "Fetching MrDocs ($mrdocs_asset)"
    rm -rf "$mrdocs_dir"
    mkdir -p "$mrdocs_dir"
    curl -fsSL --retry 3 --retry-delay 2 \
      "https://github.com/cppalliance/mrdocs/releases/download/develop-release/$mrdocs_asset" \
      -o "$mrdocs_dir/mrdocs.tar.gz"
    tar -xzf "$mrdocs_dir/mrdocs.tar.gz" -C "$mrdocs_dir"
    rm -f "$mrdocs_dir/mrdocs.tar.gz"
  fi
  mrdocs_bin=$(find "$mrdocs_dir" -type f -name mrdocs -perm -u+x | head -n 1)
  if [ -z "$mrdocs_bin" ]; then
    echo "MrDocs binary not found under $mrdocs_dir" >&2
    exit 1
  fi
  MRDOCS_ROOT=$(dirname "$(dirname "$mrdocs_bin")")
  export MRDOCS_ROOT
fi

# Install the extension into whichever MrDocs will be used, including one the
# caller supplied -- which means writing into that install. A MRDOCS_ROOT
# pointing at a read-only or shared location (/usr/local, a Nix store path) makes
# this fail, and `set -e` stops the build there; point it at a writable copy.
mkdir -p "$MRDOCS_ROOT/share/mrdocs/addons/extensions"
cp addons/extensions/*.lua "$MRDOCS_ROOT/share/mrdocs/addons/extensions/"
echo "MrDocs: $MRDOCS_ROOT (reference-snippets extension installed)"

# Later CI steps run in their own shells, so the export above does not reach
# them.
if [ -n "${GITHUB_ENV:-}" ]; then
  echo "MRDOCS_ROOT=$MRDOCS_ROOT" >> "$GITHUB_ENV"
fi

echo "Building docs..."
export PATH="$PATH:$(pwd)/node_modules/.bin"
npx antora --clean --fetch "$PLAYBOOK"
echo "Done"
