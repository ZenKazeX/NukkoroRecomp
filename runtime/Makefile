CC ?= gcc
CFLAGS ?= -O0 -Wall -I include

.PHONY: slice clean

slice: slice_add
	./slice_add

slice_add: src/slice_add.c include/gekko_rt.h
	$(CC) $(CFLAGS) src/slice_add.c -o slice_add

clean:
	rm -f slice_add
