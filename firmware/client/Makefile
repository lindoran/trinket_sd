# Normal DOS EXE build -- unlike the driver itself, this program uses the
# regular ia16-elf-gcc DOS runtime/libc (stdio, crt0), same as
# client/ifacetest.c already does. No special cmodel/freestanding flags
# needed here.

CC = ia16-elf-gcc

all: DRVTEST.EXE IFTEST.EXE SDTEST.EXE

DRVTEST.EXE: drvtest.c
	$(CC) -o $@ $<

IFTEST.EXE: ifacetest.c
	$(CC) -o $@ $<

SDTEST.EXE: sdtest.c
	$(CC) -o $@ $<

clean: 
	rm -f DRVTEST.EXE IFTEST.EXE SDTEST.EXE	
 
.PHONY: all clean
