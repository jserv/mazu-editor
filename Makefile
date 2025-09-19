CFLAGS = -Wall -std=gnu99

all: me
me: me.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(RM) me
