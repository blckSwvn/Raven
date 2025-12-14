# Compiler and flags
CC = gcc
CFLAGS = -no-pie -Wall -Wextra -O2 -fsanitize=address,undefined -g

# Include directories
INCLUDES = -I./arena -I./picohttpparser

# Target executable
TARGET = main

# Source files
SRCS = main.c picohttpparser/picohttpparser.c arena/arena.c
OBJS = $(SRCS:.c=.o)

# Default rule
all: $(TARGET)

# Link object files into executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Compile .c -> .o
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean up object files and executable
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
