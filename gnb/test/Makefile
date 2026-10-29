# Native checks of the controllable downlink policy (no gNB sources needed).
SRC = ../src
CC ?= gcc
CFLAGS = -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -I oai_stub -I $(SRC) -DMAX_MOBILES_PER_GNB=$(MAXUE)
MAXUE ?= 32
OBJS = test_ctrl.c stubs.c $(SRC)/gNB_scheduler_dlsch_ctrl_policy.c $(SRC)/nr_dl_ctrl_alloc.c $(SRC)/nr_dl_ctrl_state.c

all: test_ctrl
test_ctrl: $(OBJS) $(wildcard $(SRC)/*.h) stubs.h
	$(CC) $(CFLAGS) $(OBJS) -o $@ -lm -lpthread

# the same sources in a strict C11 configuration, as a portability check
strict: $(SRC)/nr_dl_ctrl_alloc.c $(SRC)/nr_dl_ctrl_state.c
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -c $(SRC)/nr_dl_ctrl_alloc.c -o /dev/null
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -c $(SRC)/nr_dl_ctrl_state.c -o /dev/null

tsan: $(OBJS)
	$(CC) $(CFLAGS) -fsanitize=thread $(OBJS) -o test_ctrl_tsan -lm -lpthread

asan: $(OBJS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined $(OBJS) -o test_ctrl_asan -lm -lpthread

check: test_ctrl
	./test_ctrl

clean:
	rm -f test_ctrl test_ctrl_tsan test_ctrl_asan
