# Comment/uncomment the following line to disable/enable debuging
DEBUG = y

ifeq ($(DEBUG),y)
  EXTRA_CFLAGS = -O -g -DBDTUN_DEBUG
else
  EXTRA_CFLAGS = -O2
endif

obj-m := bdtun.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
MYCFLAGS := -O2 -Wall

all: module bdtunlib.o testclient bdtun

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

testclient: bdtunlib.o testclient.c bdtun.h
	gcc $(MYCFLAGS) $(EXTRA_CFLAGS) -o testclient bdtunlib.o testclient.c

bdtun: bdtunlib.o bdtun_cli.c bdtun.h
	gcc $(MYCFLAGS) $(EXTRA_CFLAGS) -o bdtun bdtunlib.o bdtun_cli.c

bdtunlib.o: bdtunlib.c bdtun.h
	gcc $(MYCFLAGS) $(EXTRA_CFLAGS) -c -o bdtunlib.o bdtunlib.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f testclient bdtunlib.o
