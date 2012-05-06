#!/bin/bash

libtoolize -c && aclocal && autoheader && automake --add-missing -c && autoconf
