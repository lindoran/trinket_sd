# DOS EXE build. Use -mcmodel=small so code and data/stack get separate
# 64KB segments instead of cramming everything into a single 64KB Tiny segment.

CC = ia16-elf-gcc
CFLAGS = -mcmodel=small -Wall -Wextra

all: DRVTEST.EXE IFTEST.EXE SDTEST.EXE

DRVTEST.EXE: drvtest.c
	$(CC) $(CFLAGS) -o $@ $<

IFTEST.EXE: ifacetest.c
	$(CC) $(CFLAGS) -o $@ $<

# -O2 only here, not on DRVTEST.EXE/IFTEST.EXE -- this is about
# ENHANCEMENTS.md point 4 (SPP handshake tuning): sdtest.c's port-I/O
# wrappers (outportb/inportb/io_delay/ctrl_out) are all `static inline`,
# which plain GCC only actually inlines above -O0. The port I/O itself
# is protected by `asm volatile`, so -O2 can't reorder or eliminate any
# of it -- this should be a pure speed win with no correctness risk.
# Left off the other two targets since they haven't been reviewed with
# this change in mind; extend to them too once someone has.
SDTEST.EXE: sdtest.c
	$(CC) $(CFLAGS) -O2 -o $@ $<

clean: 
	rm -f DRVTEST.EXE IFTEST.EXE SDTEST.EXE	
 
.PHONY: all clean
