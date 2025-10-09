#!/bin/bash

# This script installs and starts a specific version of ClickHouse server.
# Set CH_RELEASE to the desired release. Examples:
#
# CH_RELEASE=25.9.3.48-stable
# CH_RELEASE=25.8.10.7-lts

set -e

# Fetch latest version if not specified.
# https://clickhouse.com/docs/install
if [ -z "$CH_RELEASE" ]; then
    tsv_url=https://raw.githubusercontent.com/ClickHouse/ClickHouse/master/utils/list-versions/version_date.tsv
    CH_RELEASE=$(curl -sL "$tsv_url" | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+-(stable|lts)' | sort -V -r | head -n 1)
    # List versions: curl -sL "$tsv_url" | grep -E 'stable|lts'
fi

CH_VERSION="${CH_RELEASE%-*}"
ARCH=$(dpkg --print-architecture)
base_url="https://github.com/ClickHouse/ClickHouse/releases/download/v${CH_RELEASE}"

printf "==== ClickHouse %s for %s =====\n\n" "$CH_VERSION" "$ARCH"
cd "${TMPDIR-/tmp}"

# Prevent prompt for password.
export DEBIAN_FRONTEND=noninteractive

# Install the packages.
for pkg in clickhouse-common-static clickhouse-server; do
    printf "~~~~ Installing %s ~~~~\n\n" "$pkg"
    if [ "$pkg" == 'clickhouse-server' ] && [ "${CH_VERSION%%.*}" -lt 22 ]; then
        # Prior to v22, the server package supported all architectures.
        ARCH=all
    fi
    echo "${base_url}/${pkg}_${CH_VERSION}_${ARCH}.deb"
    curl -sLo "${pkg}.deb" "${base_url}/${pkg}_${CH_VERSION}_${ARCH}.deb"
    dpkg -i "${pkg}.deb"
    rm "${pkg}.deb"
done

printf "~~~~ Starting ClickHouse %s ~~~~\n" "$CH_VERSION"
/etc/init.d/clickhouse-server start
