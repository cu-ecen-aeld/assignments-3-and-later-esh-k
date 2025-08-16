CFLAGS := -Wall
LDFLAGS := -Llib
LDLIBS := -lm
CC ?= $(TARGET_CC)

all: aesdsocket

default: all

.PHONE: all

aesdsocket: aesdsocket.o
	$(CC) $(LDFLAGS) $? $(LDLIBS) -o $@

aesdsocket.o: aesdsocket.c
	$(CC) $(CFLAGS) -c $? -o $@

clean:
	@rm -rf aesdsocket.o aesdsocket
