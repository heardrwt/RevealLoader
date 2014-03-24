#!/bin/bash
set -e

make clean
make
make package

brew install dpkg

cd releases/repo && dpkg-scanpackages ../debs | bzip2 -c > Packages.bz2