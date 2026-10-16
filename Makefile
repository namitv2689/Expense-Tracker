# Build the Expense Tracker server.
#   make        -> builds ./server (or server.exe on Windows)
#   make run    -> builds and starts it on http://localhost:8080
#   make clean  -> removes the built program

CC     ?= gcc
CFLAGS  = -Wall -Wextra -std=c99 -O2

ifeq ($(OS),Windows_NT)
  TARGET = server.exe
  LDLIBS = -lws2_32
else
  TARGET = server
  LDLIBS =
endif

$(TARGET): server.c
	$(CC) $(CFLAGS) -o $(TARGET) server.c $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f server server.exe

.PHONY: run clean
