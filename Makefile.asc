obj-m += scsi_ram.o
#EXTRA_CFLAGS += -I/usr/src/linux-2.6.37.6/drivers/scsi

KDIR := /lib/modules/$(shell uname -r)/build

PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	rm -rf *.o *.ko modules.order Module.symvers *.mod.c

