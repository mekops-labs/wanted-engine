#!/bin/bash

[ -z "$2" ] && {
	echo usage: $0 {wasm file} {output image}
	exit 1
}
[ -z "$1" ] && {
	echo Provide wasm application as first argument!
	exit 1
}

file "$1" | grep -q wasm || {
	echo $1: this is not wasm file!
	exit 1
}

TEMP=$(mktemp -d)

cp "$1" $TEMP/app.wasm
name=$(basename $1)
name="${name%%.*}"
out=$(readlink -f "$2")

pushd $TEMP

genromfs -v -V "$name" -f "$out"

popd

rm -rf $TEMP
