main: main.c
	gcc -o $@ $< -Wall -Wextra -std=c99 -O0 -g

scanner: scanner.c base.c
	gcc -o $@ $^ -Wall -Wextra -std=c99 -O0 -g

clean:
	rm -f scanner

.PHONY: main scanner clean
