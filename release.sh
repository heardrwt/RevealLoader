#!/bin/bash
set -e

make clean
make
make package

brew install dpkg

cd releases

dpkg-scanpackages debs | bzip2 -c > Packages.bz2
dpkg-scanpackages debs | gzip -c > Packages.gz

