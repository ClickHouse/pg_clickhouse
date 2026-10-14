#!/bin/sh

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
    CH_RELEASE=$(curl -sL "$tsv_url" | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+-(stable|lts)' | sort -V | tail -n 1)
    # List versions: curl -sL "$tsv_url" | grep -E 'stable|lts'
fi

CH_VERSION=${CH_RELEASE%-*}
ARCH=$(dpkg --print-architecture)
base_url=https://github.com/ClickHouse/ClickHouse/releases/download/v$CH_RELEASE
deb=${TMPDIR-/tmp}/clickhouse.deb

printf "==== ClickHouse %s for %s =====\n\n" "$CH_VERSION" "$ARCH"

# Prevent prompt for password.
export DEBIAN_FRONTEND=noninteractive

for pkg in clickhouse-common-static clickhouse-server; do
    printf "~~~~ Installing %s ~~~~\n\n" "$pkg"
    curl -sLo "$deb" "${base_url}/${pkg}_${CH_VERSION}_${ARCH}.deb"
    dpkg -i "$deb"
done
rm "$deb"

# Enable the JSON type in release prior to 2025, setting renamed in 24.8
case $CH_VERSION in
    24.8.*) setting=allow_experimental_json_type ;;
    2[0-4].*) setting=allow_experimental_object_type ;;
esac
if [ -n "$setting" ]; then
    mkdir -p /etc/clickhouse-server/users.d
    printf '<clickhouse><profiles><default><%s>1</%s></default></profiles></clickhouse>\n' \
        "$setting" "$setting" > /etc/clickhouse-server/users.d/json.xml
fi

printf "~~~~ Starting ClickHouse %s ~~~~\n" "$CH_VERSION"
/etc/init.d/clickhouse-server start

"$(dirname "$0")/lake.sh"
