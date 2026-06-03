#!/bin/bash

set -e

for fn in  src/**/*.h* src/**/*.h.in src/*.c src/**/*.c; do
    printf "%s\n" "$fn"
    pg_bsd_indent -bad -bap -bbb -bc -bl -cli1 -cp33 -cdb -nce -d0 -di12 -nfc1 -i4 -l79 -lp -lpl -nip -npro -sac -tpg -ts4 "$fn"
done
rm -f ./*.BAK
