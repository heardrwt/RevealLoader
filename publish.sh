#!/bin/bash
set -e

scp -r $(< .theos/last_package) heardrwt@ssh.rheard.com:~/domains/rheard.com/cydia/debs/

#rebuild remote
echo 'cd ~/domains/rheard.com/cydia/ && apt-ftparchive packages debs | bzip2 -c > Packages.bz2' | ssh heardrwt@ssh.rheard.com /bin/bash
echo 'cd ~/domains/rheard.com/cydia/ && apt-ftparchive packages debs | gzip -c > Packages.gz' | ssh heardrwt@ssh.rheard.com /bin/bash

