#!/bin/bash

set -e

# https://clickhouse.com/docs/install
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y apt-transport-https ca-certificates curl gnupg

# Download the ClickHouse GPG key and store it in the keyring
curl -fsSL 'https://packages.clickhouse.com/rpm/lts/repodata/repomd.xml.key' \
    | gpg --dearmor -o /usr/share/keyrings/clickhouse-keyring.gpg

# Get the system architecture
ARCH=$(dpkg --print-architecture)

# Add the ClickHouse repository to apt sources
echo "deb [signed-by=/usr/share/keyrings/clickhouse-keyring.gpg arch=${ARCH}] https://packages.clickhouse.com/deb stable main" \
    | tee /etc/apt/sources.list.d/clickhouse.list

# Update apt package lists
apt-get update -qq
apt-get install -y clickhouse-server
service clickhouse-server start
