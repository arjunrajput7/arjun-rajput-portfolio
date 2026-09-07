# Out-of-tree kbuild for the bmp280_simple I2C driver.
#
#   make                      # build against the running kernel
#   make KDIR=/path/to/kernel # build against a cross-compiled tree
#   make dtbo                 # compile the device tree overlay
#   make install / clean

obj-m += bmp280_simple.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

DTC ?= dtc

.PHONY: all modules dtbo install clean

all: modules dtbo

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

dtbo: dts/bmp280-overlay.dtbo

dts/bmp280-overlay.dtbo: dts/bmp280-overlay.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

install: modules
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f dts/bmp280-overlay.dtbo
