#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <threads.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

struct page_header {
	struct page_header *next;
	struct page_header *prev;
	size_t size_index; //either index if size_index > MAX_SIZE then is raw size
	uint64_t bitmap_size; //stores the max index of bitmap
	uint64_t bitmap[];
};

#define BINS 128
#define MAX_SIZE 2048
#define MAX_BITMAP_SIZE 4
#define SLAB_SIZE 4096

// #define DEBUG

thread_local struct page_header *freelist[BINS] = {NULL};

static inline void print_uint64_binary(uint64_t n) {
    for (int i = 63; i >= 0; i--) {  // Start from the most significant bit
        putchar((n & ((uint64_t)1 << i)) ? '1' : '0');
        if (i % 8 == 0 && i != 0) {  // Optional: add a space every 8 bits
            putchar(' ');
        }
    }
    putchar('\n');
}

static inline struct page_header *get_header(void *ptr){
	uintptr_t header = (uintptr_t)ptr;
	header &= ~((uintptr_t)0xFFF);
	return (void *)header;
}

static inline size_t align(size_t len){
	return (len + 15) & ~((size_t)15);
}

static inline void insert_page_to(struct page_header *header, struct page_header **head){
#ifdef DEBUG
#ifdef DEBUG
	printf("insert_page_to:%p, %p\n", header, head);
#endif
#endif
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

static uint64_t find_first_zero_bit(uint64_t bitmap){
	uint64_t pos = 0;
	for(uint32_t i = 0; i < 64; i++){
		if((bitmap & (1ULL << i))== 0){
			return i;
		}
	}
	return pos;
}

static inline void set_bitmap(uint64_t *bitmap, uint64_t pos, bool value){
	if(value == true)
		*bitmap |= (1ULL << pos);
	else
		*bitmap &= ~(1ULL << pos);
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
	size_t index = (len/16)-1;
	struct page_header *header = NULL;
	uint64_t found_y = 0;
	if(freelist[index]){
		for(size_t y = 0; y < MAX_BITMAP_SIZE; y++)
			if(freelist[index]->bitmap[y] != UINT64_MAX){
				header = freelist[index];
				found_y = y;
				break;
			}
	}

#ifdef DEBUG
#ifdef DEBUG
	printf("salloc\n");
#endif
#endif
	if(header){
		uint64_t bit_index = find_first_zero_bit(header->bitmap[found_y]);
		header->bitmap[found_y] |= (1ULL << bit_index);
#ifdef DEBUG
		print_uint64_binary(header->bitmap[found_y]);
#endif

		//self explanatory
		return (void *)((char *)header->bitmap+(sizeof(uint64_t)*header->bitmap_size)+((header->size_index+1)*16)*(bit_index));
	}else{
		struct page_header *new = mmap(NULL, SLAB_SIZE, 
				 PROT_READ | PROT_WRITE,
				 MAP_ANONYMOUS | MAP_PRIVATE, 
				 -1, 0);
		if(!new)return NULL;
		madvise(new, SLAB_SIZE, MADV_NOHUGEPAGE);

		uint32_t max_blocks = (SLAB_SIZE - sizeof(struct page_header))/len;
		uint32_t bitmap_bytes = (max_blocks + 7)/8;
		max_blocks = (SLAB_SIZE - sizeof(struct page_header) - bitmap_bytes)/len;
		bitmap_bytes = (max_blocks + 7)/8;

		insert_page_to(new, &freelist[index]);

		memset(new->bitmap, 0, (max_blocks+63)/64 * sizeof(uint64_t));
		new->size_index = index;
		new->bitmap_size = (max_blocks+63)/64;
		new->bitmap[0] |= (1ULL << 0);
#ifdef DEBUG
		print_uint64_binary(new->bitmap[0]);
#endif
		return(void*)((char *)new->bitmap + (sizeof(uint64_t)*new->bitmap_size));
	}

	return NULL;
}

void sfree(void *ptr){
	struct page_header *header = get_header(ptr);
#ifdef DEBUG
	printf("sfree\n");
#endif

	if(header->size_index > MAX_SIZE){
		munmap(header, header->size_index + sizeof(struct page_header));
		return;
	}
	void *header_end = ((char *)header->bitmap + sizeof(uint64_t)*header->bitmap_size);
	size_t index = (((char *)ptr - (char*)header_end)/((header->size_index+1)*16));
	size_t word_index = index / 64;
	size_t bit_index = index % 64;

	header->bitmap[word_index] &= ~(1ULL << (bit_index));
#ifdef DEBUG
	print_uint64_binary(header->bitmap[word_index]);
	printf("word_index:%zu, bit_index:%zu\n",word_index, bit_index);
#endif
}
