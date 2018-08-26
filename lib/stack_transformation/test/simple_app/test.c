#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

void f4(int a, int b)
{
	printf("%s: a: %d, b: %d\n", __func__, a, b);
	fflush(stdout);
	usleep((rand() % 10000) * 50);
}

void f3(int a, int b)
{
	printf("%s: a: %d, b: %d\n", __func__, a, b);
	fflush(stdout);
	usleep((rand() % 10000) * 50);
	f4(a * 2, b * 2);
}

void f2(int a, int b)
{
	printf("%s: a: %d, b: %d\n", __func__, a, b);
	fflush(stdout);
	usleep((rand() % 10000) * 50);
	f3(a * 2, b * 2);
}

void f1(int a, int b)
{
	printf("%s: a: %d, b: %d\n", __func__, a, b);
	fflush(stdout);
	usleep((rand() % 10000) * 50);
	f2(a * 2, b * 2);
}

int main(int argc, char *argv[])
{
	int a = 10, b = 20;

	printf("Hello, world\n");
	fflush(stdout);

	srand(time(NULL));

	for (;;) {
		f1(a, b);
		usleep((rand() % 10000) * 50);
	}

	return 0;
}
