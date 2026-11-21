ifneq ($(KERNELRELEASE),)
obj-m := coral_usb_hwmon.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
.PHONY: all clean
all clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(if $(filter all,$@),modules,clean)
endif
