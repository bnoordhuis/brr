CFLAGS = -Wall -Wextra -std=c11

.PHONY: all
all: brr

.PHONY: clean
clean:
	$(RM) brr

brr: brr.c
	$(CC) $(CFLAGS) -o $@ $^
