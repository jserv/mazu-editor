CFLAGS = -Wall -std=gnu99
LDFLAGS = -lpthread

all: me
me: me.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	$(RM) me
