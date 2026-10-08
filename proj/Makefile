CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99
TARGET  = history_tool
SRC     = command_history_tool.c
LOG     = history.log

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)
	@echo "Build complete: ./$(TARGET)"

# Smoke test. Input is piped, so the tool detects !isatty() and reads with
# fgets() instead of the termios line editor - that is what makes it scriptable.
# The arrow keys and Tab completion cannot be exercised this way; test those in
# a real terminal.
test: $(TARGET)
	@echo "--- smoke test (non-interactive) ---"
	@printf 'echo hello\nfoobar123\nls | wc -l\necho hi > out.txt\ncat out.txt\ncd /tmp\npwd\nfalse\ntrue\nretry 2 false\nhistory --failed\nstats\nexport\nexit\n' | ./$(TARGET)
	@echo "--- log field counts (every line >= 8) ---"
	@awk -F'|' '{ print NF }' $(LOG) | sort -n | uniq -c
	@rm -f out.txt /tmp/out.txt

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(LOG) history.csv out.txt

.PHONY: all test run clean
