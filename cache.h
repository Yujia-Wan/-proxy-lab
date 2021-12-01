/**
 * @file cache.h
 *
 * @author Yujia Wang <yujiawan@andrew.cmu.edu>
 */

#include "csapp.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Max cache and object sizes
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)

typedef struct cache_block {
    char *url;
    char *object;
    ssize_t object_size;
    unsigned long reference_count;
    struct cache_block *next;
    struct cache_block *prev;
} cache_block_t;

typedef struct cache {
    cache_block_t *head;
    cache_block_t *tail;
    ssize_t size;
} cache_t;

void init_cache();
void free_cache();
cache_block_t *alloc_block(const char *uri, char obj[], ssize_t obj_size);
void free_block(cache_block_t *block);
void insert_head(cache_block_t *block);
void remove_tail();
ssize_t read_cache(const char *uri, int fd);
void write_cache(const char *uri, char object[], ssize_t object_size);
void print_cache();
