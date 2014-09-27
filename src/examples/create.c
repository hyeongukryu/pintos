#include <stdio.h>
#include <syscall.h>

int
main (int argc, char **argv)
{
	create("fileName", 42);
}
