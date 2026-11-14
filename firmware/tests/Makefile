# Host tests for the hardware-independent firmware code:  make -C firmware/tests
CC ?= cc
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g -fsanitize=address,undefined -I../src

all: test_proto test_model
	./test_proto ../../protocol/vectors.txt
	./test_model

test_proto: test_proto.c ../src/proto.c ../src/proto.h
	$(CC) $(CFLAGS) -o $@ test_proto.c ../src/proto.c

test_model: test_model.c ../src/model.c ../src/proto.c ../src/model.h
	$(CC) $(CFLAGS) -o $@ test_model.c ../src/model.c ../src/proto.c

clean:
	rm -f test_proto test_model
