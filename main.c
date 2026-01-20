#include "salloc.h"
#include <stdio.h>
#include <string.h>

int main(){
	char *ptr = salloc(16);
	// sfree(ptr);
	char *ptr2 = salloc(16);
	// sfree(ptr2);
	char *ptr3 = salloc(16);
	// sfree(ptr3);
	printf("%p\n%p\n%p\n",ptr,ptr2,ptr3);


	return 0;
}
