# DOS EXE build. Use -mcmodel=small so code and data/stack get separate
# 64KB segments instead of cramming everything into a single 64KB Tiny segment.

CC = ia16-elf-gcc
CFLAGS = -mcmodel=small -Wall -Wextra

all: DRVTEST.EXE IFTEST.EXE SDTEST.EXE

DRVTEST.EXE: drvtest.c
	$(CC) $(CFLAGS) -o $@ $<

IFTEST.EXE: ifacetest.c
	$(CC) $(CFLAGS) -o $@ $<

SDTEST.EXE: sdtest.c
	$(CC) $(CFLAGS) -o $@ $<

clean: 
	rm -f DRVTEST.EXE IFTEST.EXE SDTEST.EXE	
 
.PHONY: all clean
