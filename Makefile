# Makefile for building out-of-tree kernel modules under Arch Linux (kbuild)

obj-m += nv_fermi_drv.o
nv_fermi_drv-objs := core.o vbios.o i2c.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod nv_fermi_drv.ko

unload:
	sudo rmmod nv_fermi_drv

log:
	sudo dmesg -w | grep nv_fermi_drv

.PHONY: all clean load unload log
