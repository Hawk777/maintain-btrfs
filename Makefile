maintain-btrfs : $(wildcard *.c) $(wildcard *.h)
	$(CC) -Wall -Wextra -std=c99 -D_GNU_SOURCE -pthread $(CFLAGS) -o $@ $(filter %.c,$^)

.PHONY : clean
clean :
	$(RM) -f maintain-btrfs
