ACLOCAL_AMFLAGS = -I m4

SUBDIRS = include kernel lib util

src-copy:
	mkdir -p /usr/src/bdtun-@VERSION@
	cp -R . /usr/src/bdtun-@VERSION@

src-rm:
	rm -rf /usr/src/bdtun-@VERSION@

dkms-add:
	dkms add -m bdtun -v @VERSION@

dkms-remove:
	dkms remove bdtun/@VERSION@ --all

dkms-install:
	dkms install -m bdtun -v @VERSION@

dkms-uninstall:
	dkms uninstall -m bdtun -v @VERSION@
