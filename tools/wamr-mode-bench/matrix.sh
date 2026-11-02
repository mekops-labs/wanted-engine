#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
B=$(cd "$(dirname "$0")" && pwd); E=$(cd "$B/../.." && pwd)
for rep in 1 2 3; do
 for sc in "term full 30" "term row 30" "tender full 1" "tender tile 5" "term idle 1"; do
  set -- "$sc"
  for m in classic fast; do
   "$B"/lk.sh "$E"/build-poc-$m/cmd/wanted-cli "$1" "$2" "$3" 10 hextile "$B"/lkres/$m-"$1"-"$2"-$rep > /dev/null 2>&1
  done
 done
done
echo finished > "$B"/lkres/DONE
