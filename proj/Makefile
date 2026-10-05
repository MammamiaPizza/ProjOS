CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c99
TARGET = history_tool
SRC = command_history_tool.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)
	@echo "Build complete: ./$(TARGET)"

clean:
	rm -f $(TARGET) history.log

run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	@echo "Running smoke tests..."
	@printf 'echo hello\nls -la\nhistory\nstats\ninfo\nexit\n' | ./$(TARGET)

.PHONY: all clean run test
