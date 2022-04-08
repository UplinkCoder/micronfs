#include <stdio.h>

int main(int argc, char* argv[])
{
#ifdef _MSC_VER
	printf("_MSVC_VER: %d\n", _MSC_VER);
#else
	printf("_MSC_VER is not defined\n");
#endif
	return 0;
}