ARG PG_MAJOR=19
ARG BASE=postgres

FROM $BASE AS builder
ARG PG_MAJOR

USER 0

WORKDIR /work
COPY . .
RUN set -eux && \
	# Initial system libraries
	ldconfig -p | awk '{print $NF}' | grep '^/' | sort | uniq > /tmp/base-image-libs.out && \
	# Install Dependencies
    apt-get update && apt-get install -y --no-install-recommends \
    postgresql-server-dev-$PG_MAJOR \
    libcurl4-openssl-dev \
    uuid-dev \
    make \
    libssl-dev \
    liblz4-dev \
    libzstd-dev \
    libicu-dev \
    g++

RUN make && make install DESTDIR=/dest

# Gather dependent system libraries and their licenses; based on
# https://github.com/cloudnative-pg/postgres-extensions-containers/blob/main/postgis/Dockerfile
RUN mkdir -p /system /licenses && \
	# Get libraries
	ldd /dest/usr/lib/postgresql/$PG_MAJOR/lib/pg_clickhouse*.so \
		| awk '{print $3}' | grep '^/' | sort | uniq > /tmp/all-deps.out && \
	# Extract all the libs that aren't already part of the base image
	comm -13 /tmp/base-image-libs.out /tmp/all-deps.out > /tmp/libraries.out && \
	while read -r lib; do \
		resolved=$(readlink -f "$lib"); \
		dir=$(dirname "$lib"); \
		base=$(basename "$lib"); \
		# Copy the real file
		cp -a "$resolved" /system/; \
		# Reconstruct all its symlinks
		for file in "$dir"/"${base%.so*}.so"*; do \
			[ -e "$file" ] || continue; \
			# If it's a symlink and it resolves to the same real file, we reconstruct it
			if [ -L "$file" ] && [ "$(readlink -f "$file")" = "$resolved" ]; then \
				ln -sf "$(basename "$resolved")" "/system/$(basename "$file")"; \
			fi; \
		done; \
	done < /tmp/libraries.out && \
	# Get licenses
	for lib in $(find /system -maxdepth 1 -type f -name '*.so*'); do \
		# Get the name of the pkg that installed the library
		pkg=$(dpkg -S "$(basename "$lib")" | grep -v "diversion by" | awk -F: '/:/{print $1; exit}'); \
		[ -z "$pkg" ] && continue; \
		mkdir -p "/licenses/$pkg" && cp -a "/usr/share/doc/$pkg/copyright" "/licenses/$pkg/copyright"; \
	done

FROM scratch
ARG VERSION
ARG PG_MAJOR

# Licenses
COPY --from=builder /licenses /licenses/
COPY LICENSE.md /licenses/postgresql-$PG_MAJOR-pg_clickhouse-$VERSION/copyright

# Libraries
COPY --from=builder /dest/usr/lib/postgresql/$PG_MAJOR/lib /lib

# Share
COPY --from=builder /dest/usr/share/postgresql/$PG_MAJOR/extension /share/extension

# System libs
COPY --from=builder /system /system/

USER 65532:65532
