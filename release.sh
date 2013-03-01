#!/bin/bash

./bootstrap.sh
./configure
make distclean
tar -czf release.tar.gz --exclude=*.disk --exclude=release.tar.gz \
--exclude=.git --exclude=.gitignore --exclude=*.cache *
