CC = gcc
CFLAGS = -Wall -std=c11
TARGET = npshell
SRCS = npshell.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)