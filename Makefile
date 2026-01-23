CC = gcc
CFLAGS = -Wall -Wextra -O2 -fsanitize=address,undefined -g -pthread
INCLUDES = ./salloc
TARGET = main

SRCS = main.c salloc.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJS) $(TARGET)

.PHONY: all clean
