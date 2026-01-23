#include "salloc.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

void *ptr;

void *remote_free(void *argc){
	sfree(ptr);
	return NULL;
}

int main(){
	ptr = salloc(16);
	pthread_t tid;
	pthread_create(&tid, NULL, remote_free, NULL);
	pthread_join(tid, NULL);
	void *ptr2 = salloc(16);
	sfree(ptr2);
	return 0;
}
