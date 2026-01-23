#include <stdalign.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <threads.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdatomic.h>

#define DEBUG

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
	void *owner;
	size_t size_index; //either index if size_index > MAX_SIZE then is raw size
	uint32_t blocks_used;
	alignas(64)_Atomic (struct block_header*)remote_head;
};

thread_local uint8_t thread_owner;
thread_local struct page_header *freelist[BINS] = {NULL};

_Atomic (struct page_header*)global[BINS] = {NULL};

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
	struct block_header *prev = NULL;
	uint64_t i = 0;
	while((char *)block+size <= (char *)page_end){
		insert_to_head(block, new);
		prev = block;
		block = (void *)((char *)block + size);
		i++;
	}
	if(prev)prev->next = NULL;

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
	if(freelist[i]){
		if(freelist[i]->head){
#ifdef DEBUG
		printf("local salloc\n");
#endif
		struct block_header *block = freelist[i]->head;
		freelist[i]->head = block->next;
		freelist[i]->blocks_used++;
		return (void *)block;
		}else if(atomic_load(&freelist[i]->remote_head)){
#ifdef DEBUG
			printf("merging remote list\n");
#endif
			struct block_header *old_head;
			do{
				old_head = atomic_load_explicit(&freelist[i]->remote_head, memory_order_acquire);
				if(!old_head)break;
			}while(!atomic_compare_exchange_weak_explicit(&freelist[i]->remote_head, &old_head, NULL, memory_order_acquire, memory_order_relaxed));
			struct block_header *curr = old_head;
			struct block_header *next;
			while(curr){
				next = curr->next;
				insert_to_head(curr, freelist[i]);
				curr = next;
			}
			freelist[i]->blocks_used++;
			return old_head;
		}else if(global[i]){
#ifdef DEBUG
			printf("taking from global\n");
#endif
			struct page_header *old_head;
			struct page_header *new_head;
			do{
				old_head = atomic_load_explicit(&global[i], memory_order_acquire);
				if(!old_head)break;
				new_head = old_head->next;
			}while(!atomic_compare_exchange_weak_explicit(&global[i], &old_head, new_head, memory_order_acquire, memory_order_relaxed));
			old_head->owner = &thread_owner;
			old_head->blocks_used = 0;
			insert_page_to(old_head, &freelist[i]);
			return salloc(len);
		}
	}else{
#ifdef DEBUG
		printf("mmaping new page\n"); 
#endif
		struct page_header *new = mmap(NULL, SLAB_SIZE, 
				 PROT_READ | PROT_WRITE,
				 MAP_ANONYMOUS | MAP_PRIVATE, 
				 -1, 0);
		if(!new)return NULL;
		madvise(new, SLAB_SIZE, MADV_NOHUGEPAGE);
		new->size_index = i;
		new->blocks_used = 1;
		new->owner = &thread_owner;
		atomic_store(&new->remote_head, NULL);
		populate(new, len); //first block is reserved no need to rm_from_free
		return (void *)((char *)new+sizeof(struct page_header));
	}

	return NULL;
}

void sfree(void *ptr){
	struct page_header *header = get_header(ptr);
	if(header->owner != &thread_owner){
#ifdef DEBUG
		printf("remote free\n");
#endif
		if(header->size_index > MAX_SIZE){
#ifdef DEBUG
			printf("munmapped\n");
#endif
			munmap(header, header->size_index + sizeof(struct page_header));
			return;
		}
			struct block_header *new_head = ptr;
			struct block_header *old_head;
		do{
			old_head = atomic_load_explicit(&header->remote_head, memory_order_relaxed);
			if(!old_head)break;
			new_head->next = old_head;
		}while(!atomic_compare_exchange_weak_explicit(&header->remote_head, &old_head, new_head, memory_order_release, memory_order_relaxed));
		return;
	}
	if(header->size_index > MAX_SIZE){
#ifdef DEBUG
		printf("munmapped\n");
#endif
		munmap(header, header->size_index + sizeof(struct page_header));
		return;
	}

	header->blocks_used--;
	insert_to_head((struct block_header *)ptr, header);
	if(header->blocks_used == 0){
#ifdef DEBUG
		printf("inserted to global\n");
#endif
		//whole page/slab is free
		rm_page_from(header, &freelist[header->size_index]);
		struct page_header *old_head;
		do{
			old_head = atomic_load_explicit(&global[header->size_index], memory_order_relaxed);
			if(!old_head)break;
			header->next = old_head;
		}while(!atomic_compare_exchange_weak_explicit(&global[header->size_index], &old_head, header, memory_order_release, memory_order_relaxed));
		return;
	}
	return;
}
