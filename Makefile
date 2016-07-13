obj-m := block.o
EXTRA_CFLAGS += -O3
block-objs := block_main.o

# Otherwise we were called directly from the command
# line; invoke the kernel build system.
#else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
        $(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
        rm -rf .block.* .tmp_versions *.ko *.o Module.symvers Module.markers modules.order *mod.c .block.o.cmd .block.mod.o.cmd block.ko.unsigned

load:
        echo 8 > /proc/sys/kernel/printk
        insmod block.ko size=2048

unload:
        rmmod block

#endif
