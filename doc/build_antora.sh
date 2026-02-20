#!/bin/bash
#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
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

echo "Building docs..."
export PATH="$PATH:$(pwd)/node_modules/.bin"
npx antora --clean --fetch "$PLAYBOOK"
echo "Done"
