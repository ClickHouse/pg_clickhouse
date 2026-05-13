ClickHouse Version Testing Containers
=====================================

This directory's [docker-compose.yml](docker-compose.yml) starts images for
each supported major version of ClickHouse. Each container also runs
PostgreSQL 18. Both ClickHouse and PostgreSQL run on their default ports
(unexported) and trust all local connections. The containers are based on
[pgxn-tools].

Example usage:

```sh
make start
docker logs -f ch_25_11
# Wait a few minutes for the services to start (log output will stop)
docker exec -it ch_25_11 bash
pg-build-test # or make && make install && make installcheck
```

The containers should remain until you `make stop`; if you restart Docker (or
your system), they may need to be started again, e.g., `docker start ch_25_11`.

  [pgxn-tools]: https://github.com/pgxn/docker-pgxn-tools/
