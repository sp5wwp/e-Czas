e-czas-decode: main.c
	gcc -O2 -Wall -Wextra main.c -o e-czas-decode -lm

clean:
	rm e-czas-decode
