#ifndef _HASHTABLE_H_
#define _HASHTABLE_H_

struct hashtable {
    struct hashtable_entry *list;
    uint32_t slots;
};

struct hashtable_entry {
    char *key;
    void *value;
    struct hashtable_entry *next;
};

struct hashtable *hashtable_new(uint32_t slots);

int hashtable_insert(struct hashtable *ht, char *key, void *value);

int hashtable_remove(struct hashtable *ht, char *key);

struct hashtable_entry *hashtable_get(struct hashtable *ht, char *key);

#endif
