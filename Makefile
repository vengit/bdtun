obj-m := bdtun.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	$(MAKE) testclient

testclient: testclient.c
	gcc -o testclient testclient.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f testclient
