#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <threads.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#define BINS 128
#define MAX_SIZE 2048
#define SLAB_SIZE 4096

struct block_header {
	struct block_header *next;
};

struct page_header {
	struct page_header *next;
	struct page_header *prev;
	struct block_header *head;
	size_t size_index; //either index if size_index > MAX_SIZE then is raw size
	uint32_t blocks_used;
	uint32_t placeholder; //for alignment
};

// #define DEBUG

thread_local struct page_header *freelist[BINS] = {NULL};

static inline struct page_header *get_header(void *ptr){
	uintptr_t header = (uintptr_t)ptr;
	header &= ~((uintptr_t)0xFFF);
	return (void *)header;
}

static inline size_t align(size_t len){
	return (len + 15) & ~((size_t)15);
}

static inline void insert_page_to(struct page_header *header, struct page_header **head){
	header->prev = NULL;
	if(*head)(*head)->prev = header;
	header->next = *head;
	*head = header;
}

static inline void rm_page_from(struct page_header *header, struct page_header **head){
	if(header->prev)header->prev->next = header->next;
	if(header->next)header->next->prev = header->prev;
	if(*head == header)*head = header->next;
}

static inline void insert_to_head(struct block_header *ptr, struct page_header *header){
	ptr->next = header->head;
	header->head = ptr;
}

void populate(struct page_header *new, uint32_t size){
	struct block_header *block = (struct block_header *)((char *)new + sizeof(struct page_header) + size); //skips first block to avoid pop_from_head for first allocation
	void *page_end = ((char *)new + SLAB_SIZE);
	uint64_t i = 0;
	while(block && (void*)((char*)block+size) < page_end){
		insert_to_head(block, new);
		block = (void *)((char *)block + size);
		i++;
	}
	printf("%zu\n",i);
	block->next = NULL;
	insert_page_to(new, &freelist[new->size_index]);
}

void *salloc(size_t len){
	len = align(len);
	if(len > MAX_SIZE){
		struct page_header *new = mmap(NULL, len + sizeof(struct page_header),
				 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 
				 -1, 0);
		if(!new)return NULL;
		new->size_index = len;
		return (void *)((char *)new + sizeof(struct page_header));
	}
	size_t i = (len/16)-1;
	if(freelist[i] && freelist[i]->head){
#ifdef DEBUG
		printf("salloc\n");
#endif
		struct block_header *block = freelist[i]->head;
		freelist[i]->head = block->next;
		freelist[i]->blocks_used++;
		return (void *)block;
	}else{
		struct page_header *new = mmap(NULL, SLAB_SIZE, 
				 PROT_READ | PROT_WRITE,
				 MAP_ANONYMOUS | MAP_PRIVATE, 
				 -1, 0);
		if(!new)return NULL;
		madvise(new, SLAB_SIZE, MADV_NOHUGEPAGE);
		new->size_index = i;
		new->blocks_used = 1;
		populate(new, len); //first block is reserved no need to rm_from_free
		return (void *)((char *)new+sizeof(struct page_header));
	}

	return NULL;
}

void sfree(void *ptr){
	struct page_header *header = get_header(ptr);

	if(header->size_index > MAX_SIZE){
		munmap(header, header->size_index + sizeof(struct page_header));
		return;
	}

	header->blocks_used--;
	insert_to_head((struct block_header *)ptr, header);
	if(header->blocks_used == 0){
		//whole page/slab is free
	}
	return;
}
