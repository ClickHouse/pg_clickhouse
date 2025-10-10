#include <string.h>
#include <internal.h>

int ends_with(const char *s, const char *suffix) {
	size_t slen = strlen(s);
	size_t suffix_len = strlen(suffix);
	return suffix_len <= slen && !strcmp(s + slen - suffix_len, suffix);
}

/*
 * Check whether the given string matches a ClickHouse Cloud host name.
 */
int ch_is_cloud_host(const char *host)
{
	if (!host) return 0;
	return ends_with(host, ".clickhouse.cloud")
		|| ends_with(host, ".clickhouse-staging.com")
		|| ends_with(host, ".clickhouse-dev.com");
}
