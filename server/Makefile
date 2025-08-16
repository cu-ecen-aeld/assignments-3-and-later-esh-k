CFLAGS ?= -Wall
LDFLAGS ?= -lpthread -lrt -Llib
LDLIBS ?= -lm
CC ?= $(CROSS_COMPILE)gcc
TARGET ?= aesdsocket

all: $(TARGET)

default: all

.PHONE: all

aesdsocket: aesdsocket.o
	$(CC) $(LDFLAGS) $? $(LDLIBS) -o $@

aesdsocket.o: aesdsocket.c
	$(CC) $(CFLAGS) -c $? -o $@

clean:
	@rm -rf aesdsocket.o aesdsocket
