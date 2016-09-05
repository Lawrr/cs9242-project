#include <string.h>
#include <cspace/cspace.h>

#include "hashtable.h"

static uint32_t hash(char *key, uint32_t slots) {
    /* fnv32 hash */
    unsigned hash = 2166136261U;
    for (; *key; key++)
        hash = (hash ^ *key) * 0x01000193;
    return hash % slots;
}

//TODO just insert into head
static int list_insert(struct hashtable_entry *entry_head,
                       struct hashtable_entry *new_entry) {
    struct hashtable_entry *curr_entry = entry_head;
    if (curr_entry == NULL || curr_entry->key == NULL) {
        /* First entry in list */
        *curr_entry = *new_entry;
    } else {
        while (curr_entry->next != NULL) {
            curr_entry = curr_entry->next;
        }
        curr_entry->next = new_entry;
    }

    return 0;
}

static struct hashtable_entry *list_get(struct hashtable_entry *entry_head,
                                        char *key) {
    struct hashtable_entry *curr_entry = entry_head;
    while (curr_entry != NULL && curr_entry->key != NULL) {
        
	if (!strcmp(curr_entry->key, key)) {
            return curr_entry;
        }
        curr_entry = curr_entry->next;
    }

    return NULL;
}

static int *list_remove(struct hashtable_entry *entry_head,
                                        char *key) {
    struct hashtable_entry *prev = NULL;
    struct hashtable_entry *curr = entry_head;
    while (curr != NULL && curr->key != NULL) {
        if (!strcmp(curr->key, key)) {
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    if (curr == NULL) return 1;
    if (curr != NULL) {
        if (prev == NULL) {
            if (entry_head->next != NULL) {
                *entry_head = *curr->next;
                entry_head->next = curr->next;
                free(curr);
            } else {
                entry_head->key = NULL;
                entry_head->value = NULL;
                entry_head->next = NULL;
            }
        } else {
            prev->next = curr->next;
            free(curr);
        }
    }
    return 0;
}

struct hashtable *hashtable_new(uint32_t slots) {
    struct hashtable *ht = malloc(sizeof(struct hashtable));
    if (ht == NULL) {
        return NULL;
    }

    ht->slots = slots;

    ht->list = malloc(slots * sizeof(struct hashtable_entry));
    if (ht->list == NULL) {
        return NULL;
    }
    for (int i = 0; i < slots; i++) {
        ht->list[i].key = NULL;
        ht->list[i].value = NULL;
        ht->list[i].next = NULL;
    }

    return ht;
}

/* Assumes entry to insert is not already in the hashtable */
int hashtable_insert(struct hashtable *ht, char *key, void *value) {
    uint32_t index = hash(key, ht->slots);
    struct hashtable_entry *entry = malloc(sizeof(struct hashtable_entry));
    entry->key = key;
    entry->value = value;
    entry->next = NULL;

    int err = list_insert(&ht->list[index], entry);
    return err;
}

int hashtable_remove(struct hashtable *ht, char *key) {
    uint32_t index = hash(key, ht->slots);

    int err = list_remove(&ht->list[index], key);
    return err;
}

struct hashtable_entry *hashtable_get(struct hashtable *ht, char *key) {
    uint32_t index = hash(key, ht->slots);
    return list_get(&ht->list[index], key);
}
