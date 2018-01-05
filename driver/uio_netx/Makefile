obj-m := uio_netx.o

ifneq ($KDIR, "")
	KDIR := /lib/modules/$(shell uname -r)/build
endif	

ifdef DMA_DISABLE
	ccflags-y := 
else
	ccflags-y := -DDMA_SUPPORT
endif

PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	rm -f *.o *.ko *.order *.mod* Module.symvers
