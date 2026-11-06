main: main.c
	gcc -o $@ $< -Wall -Wextra -std=c99 -O0 -g

clean:
	rm -f main

.PHONY: main clean
