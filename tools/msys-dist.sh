#!/bin/bash

if [[ ! -x melonDSHD.exe ]]; then
	echo "Run this script from the directory you built melonDSHD."
	exit 1
fi

mkdir -p dist

for lib in $(ldd melonDSHD.exe | grep mingw | sed "s/.*=> //" | sed "s/(.*)//"); do
	cp "${lib}" dist
done

cp melonDSHD.exe dist
windeployqt dist
