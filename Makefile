obj-m := bdtun.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	$(MAKE) tenmegdisk

tenmegdisk: tenmegdisk.c
	gcc -o tenmegdisk tenmegdisk.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm tenmegdisk
