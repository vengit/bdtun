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

all: module

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install: module
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: uninstall

uninstall:
	echo "Not yet implemented, sorry."

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
