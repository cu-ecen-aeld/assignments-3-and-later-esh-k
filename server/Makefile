CFLAGS := -Wall
LDFLAGS := -Llib
LDLIBS := -lm

all: aesdsocket

default: all

.PHONE: all

aesdsocket: aesdsocket.o
	$(CC) $(LDFLAGS) $? $(LDLIBS) -o $@

aesdsocket.o: aesdsocket.c
	$(CC) $(CFLAGS) -c $? -o $@

clean:
	@rm -rf aesdsocket.o aesdsocket
