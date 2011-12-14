obj-m := bdtun.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
MYCFLAGS := -O2 -Wall

all: module bdtunlib.o testclient cli

module: bdtun.c bdtun.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

testclient: bdtunlib.o testclient.c bdtun.h
	gcc $(MYCFLAGS) -o testclient bdtunlib.o testclient.c

cli: bdtunlib.o bdtun_cli.c bdtun.h
	gcc $(MYCFLAGS) -o bdtun bdtunlib.o bdtun_cli.c

bdtunlib.o: bdtunlib.c bdtun.h
	gcc $(MYCFLAGS) -c -o bdtunlib.o bdtunlib.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f testclient bdtunlib.o
