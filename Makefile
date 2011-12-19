ifeq ($(DEBUG),y)
  EXTRA_CFLAGS = -O -g -DBDTUN_DEBUG
else
  EXTRA_CFLAGS = -O2
endif

obj-m := bdtun.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
MYCFLAGS := -Wall -L.

MYCFLAGS += $(EXTRA_CFLAGS)

all: module libbdtun.so testclient bdtun

install: module libbdtun.so bdtun
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	mkdir -p /usr/local/lib
	mkdir -p /usr/local/bin
	cp libbdtun.so /usr/local/lib/libbdtun.so
	cp bdtun /usr/local/bin/bdtun
	ldconfig
	depmod

uninstall:
	rm -f /usr/local/lib/libbdtun.so
	rm -f /usr/local/bin/bdtun
	ldconfig

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

testclient: libbdtun.so testclient.c bdtun.h
	gcc $(MYCFLAGS) -o testclient -lbdtun testclient.c

bdtun: libbdtun.so bdtun_cli.c bdtun.h
	gcc $(MYCFLAGS) -o bdtun -lbdtun bdtun_cli.c

libbdtun.so: libbdtun.c bdtun.h
	gcc $(MYCFLAGS) -shared -c -o libbdtun.so libbdtun.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f testclient bdtun libbdtun.so
