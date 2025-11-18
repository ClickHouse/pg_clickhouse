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
    default = [18, 17, 16, 15, 14, 13]
}

# Values to use in the targets.
now = timestamp()
authors = "David E. Wheeler"
url = "https://github.com/ClickHouse/pg_clickhouse"

target "default" {
  platforms = ["linux/amd64", "linux/arm64"]
  matrix = {
    pgv = pg_versions
  }
  name = "pg_clickhouse-${pgv}"
  context = "."
  args = {
    PG_MAJOR = "${pgv}"
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
    "index,manifest:org.opencontainers.image.title=PostgreSQL with pg_clickhouse",
    "index,manifest:org.opencontainers.image.description=PostgreSQL ${pgv} with pg_clickhouse ${version}",
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
    "org.opencontainers.image.title" = "PostgreSQL with pg_clickhouse",
    "org.opencontainers.image.description" = "PostgreSQL ${pgv} with pg_clickhouse ${version}",
    "org.opencontainers.image.documentation" = "${url}",
    "org.opencontainers.image.authors" = "${authors}",
    "org.opencontainers.image.licenses" = "PostgreSQL AND Apache-2.0"
    "org.opencontainers.image.base.name" = "scratch",
  }
}
