include Makefile.config

LIBRARY_SOURCES=$(wildcard library/*.c)
LIBRARY_HEADERS=$(wildcard library/*.h)
USER_SOURCES=$(wildcard user/*.c)
USER_PROGRAMS=$(USER_SOURCES:c=exe)
KERNEL_SOURCES=$(wildcard kernel/*.[chS])
WORDS=/usr/share/dict/words

.PHONY: clean build-kernel build-library build-userspace build-cdrom-image 

all: build-cdrom-image

build-kernel: kernel/basekernel.img

build-library: library/baselib.a

build-userspace: $(USER_PROGRAMS)

build-cdrom-image: basekernel.iso

kernel/basekernel.img: $(KERNEL_SOURCES) $(LIBRARY_HEADERS)
	cd kernel && make
	cp kernel/basekernel.img ../NexShell.krn

library/baselib.a: $(LIBRARY_SOURCES) $(LIBRARY_HEADERS)
	cd library && make

$(USER_PROGRAMS): $(USER_SOURCES) library/baselib.a $(LIBRARY_HEADERS)
	cd user && make

image: kernel/basekernel.img $(USER_PROGRAMS)
	rm -rf image 
	mkdir -p image/boot image/shell 
	cp kernel/basekernel.img image/boot 
	@echo "------------------------------------------------"
	@echo "Folders 'bin' and 'data' removed."
	@echo "Folder 'shell' created for DoorsOS."
	@echo "PLEASE DO WHAT YOU NEED IN THE FOLDER NOW."
	@echo "------------------------------------------------"
	@echo "Press Enter in this window to continue..."
	@/bin/bash -c "read"

basekernel.iso: image
	${ISOGEN} -input-charset utf-8 -iso-level 2 -J -R -o $@ -b boot/basekernel.img image

disk.img:
	qemu-img create disk.img 10M

run: basekernel.iso disk.img
	qemu-system-i386 -cdrom basekernel.iso -hda disk.img 

debug: basekernel.iso disk.img
	qemu-system-i386 -cdrom basekernel.iso -hda disk.img -s -S &

clean:
	rm -rf basekernel.iso image disk.img
	cd kernel && make clean
	cd library && make clean
	cd user && make clean
