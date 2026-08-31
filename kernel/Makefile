obj-m += quark_kernel.o

# Build using the running kernel build system.
#
# `all` builds the release module: VM detection is enforced unconditionally,
# and the code path that would bypass it is not compiled in at all.
#
# `testing` additionally defines QUARK_TESTING_BUILD, which compiles in the
# VM-check bypass -- needed on hosts that are themselves hypervisor guests
# (e.g. this project's own VM test rig), never intended for a real release.
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) modules

testing:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) modules ccflags-y="-DQUARK_TESTING_BUILD"

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean
