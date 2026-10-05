# Variables to be specified externally.
variable "registry" {
  default = "ghcr.io/clickhouse"
  description = "The image registry."
}

variable "version" {
  default = ""
  description = "The release version."
}

variable "revision" {
  default = ""
  description = "The current Git commit SHA."
}

# Postgres versions to build. Pass comma-delimited list: pg_versions=18,16.
variable "pg_versions" {
    type    = list(number)
    default = [19, 18, 17, 16, 15, 14]
}

variable "os_name" {
  default = "trixie"
  description = "The name of the base Debian distribution."
}

# Values to use in the targets.
now = timestamp()
authors = "David E. Wheeler"
url = "https://github.com/ClickHouse/pg_clickhouse"

group "default" {
  # Exclude cnpg if not building for Postgres 18 or higher.
  targets = length([for v in pg_versions : v if v >= 18]) > 0 ? ["pg_clickhouse", "cnpg"] : ["pg_clickhouse"]
}

target "pg_clickhouse" {
  platforms = ["linux/amd64", "linux/arm64"]
  matrix = {
    pgv = pg_versions
  }
  name = "pg_clickhouse-${pgv}"
  context = "."
  dockerfile = "dist/pg.Dockerfile"
  args = {
    PG_MAJOR = "${pgv}"
    BASE = "postgres:${base_version("${pgv}")}-${os_name}"
  }
  tags = [
    "${registry}/pg_clickhouse:${pgv}",
    "${registry}/pg_clickhouse:${pgv}-${version}",
  ]
  annotations = [
    "index,manifest:org.opencontainers.image.created=${now}",
    "index,manifest:org.opencontainers.image.url=${url}",
    "index,manifest:org.opencontainers.image.source=${url}",
    "index,manifest:org.opencontainers.image.version=${pgv}-${version}",
    "index,manifest:org.opencontainers.image.revision=${revision}",
    "index,manifest:org.opencontainers.image.vendor=${authors}",
    "index,manifest:org.opencontainers.image.title=PostgreSQL ${pgv} with pg_clickhouse ${version}",
    "index,manifest:org.opencontainers.image.description=PostgreSQL ${pgv} with pg_clickhouse ${version} on ${os_name}",
    "index,manifest:org.opencontainers.image.documentation=${url}",
    "index,manifest:org.opencontainers.image.authors=${authors}",
    "index,manifest:org.opencontainers.image.licenses=PostgreSQL AND Apache-2.0",
    "index,manifest:org.opencontainers.image.base.name=postgres",
  ]
  labels = {
    "org.opencontainers.image.created" = "${now}",
    "org.opencontainers.image.url" = "${url}",
    "org.opencontainers.image.source" = "${url}",
    "org.opencontainers.image.version" = "${pgv}-${version}",
    "org.opencontainers.image.revision" = "${revision}",
    "org.opencontainers.image.vendor" = "${authors}",
    "org.opencontainers.image.title" = "PostgreSQL ${pgv} with pg_clickhouse ${version}",
    "org.opencontainers.image.description" = "PostgreSQL ${pgv} with pg_clickhouse ${version} on ${os_name}",
    "org.opencontainers.image.documentation" = "${url}",
    "org.opencontainers.image.authors" = "${authors}",
    "org.opencontainers.image.licenses" = "PostgreSQL AND Apache-2.0"
    "org.opencontainers.image.base.name" = "postgres",
  }
}

target "cnpg" {
  platforms = ["linux/amd64", "linux/arm64"]
  matrix = {
    pgv = [for v in pg_versions : v if v >= 18]
  }
  name = "cnpg-pg_clickhouse-${pgv}"
  context = "."
  tags = [
    "${registry}/cnpg-pg_clickhouse:${pgv}",
    "${registry}/cnpg-pg_clickhouse:${pgv}-${version}",
  ]
  dockerfile = "dist/cnpg.Dockerfile"
  args = {
    PG_MAJOR = "${pgv}"
    BASE = "ghcr.io/cloudnative-pg/postgresql:${base_version("${pgv}")}-minimal-${os_name}"
    VERSION = "${version}"
  }
  annotations = [
    "index,manifest:org.opencontainers.image.created=${now}",
    "index,manifest:org.opencontainers.image.url=${url}",
    "index,manifest:org.opencontainers.image.source=${url}",
    "index,manifest:org.opencontainers.image.version=${version}",
    "index,manifest:org.opencontainers.image.revision=${revision}",
    "index,manifest:org.opencontainers.image.vendor=${authors}",
    "index,manifest:org.opencontainers.image.title=pg_clickhouse ${version} ${os_name}",
    "index,manifest:org.opencontainers.image.description=pg_clickhouse ${version} container image for PostgreSQL ${pgv} on ${os_name}",
    "index,manifest:org.opencontainers.image.documentation=${url}",
    "index,manifest:org.opencontainers.image.authors=${authors}",
    "index,manifest:org.opencontainers.image.licenses=PostgreSQL AND Apache-2.0",
    "index,manifest:org.opencontainers.image.base.name=scratch",
    "index,manifest:io.cloudnativepg.image.base.name=ghcr.io/cloudnative-pg/postgresql:${pgv}-minimal-${os_name}",
    "index,manifest:io.cloudnativepg.image.base.os=${os_name}",
    "index,manifest:io.cloudnativepg.image.base.pgmajor=${pgv}",
    "index,manifest:io.cloudnativepg.image.sql.version=${version}",
  ]
  labels = {
    "org.opencontainers.image.created" = "${now}",
    "org.opencontainers.image.url" = "${url}",
    "org.opencontainers.image.source" = "${url}",
    "org.opencontainers.image.version" = "${version}",
    "org.opencontainers.image.revision" = "${revision}",
    "org.opencontainers.image.vendor" = "${authors}",
    "org.opencontainers.image.title" = "pg_clickhouse ${version} ${os_name}",
    "org.opencontainers.image.description" = "pg_clickhouse ${version} container image for PostgreSQL ${pgv} on ${os_name}",
    "org.opencontainers.image.documentation" = "${url}",
    "org.opencontainers.image.authors" = "${authors}",
    "org.opencontainers.image.licenses" = "PostgreSQL AND Apache-2.0"
    "org.opencontainers.image.base.name" = "scratch",
    "io.cloudnativepg.image.base.name" = "ghcr.io/cloudnative-pg/postgresql:${pgv}-minimal-${os_name}",
    "io.cloudnativepg.image.base.os" = "${os_name}",
    "io.cloudnativepg.image.base.pgmajor" = "${pgv}",
    "io.cloudnativepg.image.sql.version" = "${version}",
  }
}

# Determines the version string used in image tags. Required because beta
# versions have "betaX" in the version name, whereas final releases do not.
# Must be updated upon the release of 19.0 and the introduction of 20beta1.
function base_version {
  params = [ pgv ]
  result =  pgv == 19 ? "${pgv}beta4" : pgv
}
