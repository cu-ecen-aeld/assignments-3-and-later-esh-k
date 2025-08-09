CFLAGS := -Wall
LDFLAGS := -Llib
LDLIBS := -lm

CROSS_COMPILE := 
CC := $(CROSS_COMPILE)$(CC)

ALL: writer

.PHONY: ALL

writer: writer.o
	$(CC) $(LDFLAGS) $? $(LDLIBS) -o $@

writer.o: writer.c
	$(CC) $(CFLAGS) -c $? -o $@

clean:
	@rm -rf writer.o writer
