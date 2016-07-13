obj-m := block_test.o
EXTRA_CFLAGS += -O3

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	rm -rf .block_test.* .tmp_versions *.ko *.o Module.symvers Module.markers modules.order *mod.c .block_test.o.cmd .block_test.mod.o.cmd block_test.ko.unsigned

load:
	echo 8 > /proc/sys/kernel/printk
	insmod block_test.ko size=2048

unload:
	rmmod block_test