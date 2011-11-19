obj-m := bdtun.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all: module lib testclient

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

testclient: lib testclient.c
	gcc -o testclient bdtunlib.o testclient.c

cli: lib bdtun_cli.c
	gcc -o bdtun bdtunlib.o bdtun_cli.c

lib:
	gcc -c -o bdtunlib.o bdtunlib.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f testclient
