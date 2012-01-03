ifeq ($(DEBUG),y)
  EXTRA_CFLAGS = -O -g -DBDTUN_DEBUG
else
  EXTRA_CFLAGS = -O2
endif

modname = bdtun
koname = $(modname).ko
obj-m := $(modname).o
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

uninstall:
	rm -f $(KDIR)/extra/$(koname)
	depmod -a

distclean: clean

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
