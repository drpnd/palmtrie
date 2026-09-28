/*_
 * Copyright (c) 2015-2020 Hirochika Asai <asai@jar.jp>
 * All rights reserved.
 */

#include "palmtrie.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Recursively delete the node in a tree
 */
static void
_delete_node(struct palmtrie_tpt_node *n)
{
    if ( NULL == n ) {
        return;
    }

    if ( NULL != n->left ) {
        if ( n->bit > n->left->bit ) {
            _delete_node(n->left);
        }
    }
    if ( NULL != n->center ) {
        if ( n->bit > n->center->bit ) {
            _delete_node(n->center);
        }
    }
    if ( NULL != n->right ) {
        if ( n->bit > n->right->bit ) {
            _delete_node(n->right);
        }
    }

    free(n);

    return;
}

/*
 * Release the instance
 */
int
palmtrie_tpt_release(struct palmtrie *palmtrie)
{
    /* Check the type */
    if ( PALMTRIE_BASIC != palmtrie->type ) {
        /* Type error */
        return -1;
    }
    /* Recursively delete nodes from the root */
    _delete_node(palmtrie->u.tpt.root);

    return 0;
}

/*
 * Add a leaf to the trie
 */
static int
_add_leaf(struct palmtrie_tpt_node **node, addr_t addr, addr_t mask, int priority,
          void *data, int bit)
{
    struct palmtrie_tpt_node *n;

    /* Allocate a node */
    n = malloc(sizeof(struct palmtrie_tpt_node));
    if ( NULL == n ) {
        return -1;
    }
    n->bit = 0;
    n->addr = addr;
    n->mask = mask;
    n->priority = priority;
#if PALMTRIE_PRIORITY_SKIP
    if ( NULL != *node ) {
        n->max_priority = (*node)->max_priority > priority
            ? (*node)->max_priority : priority;
    } else {
        n->max_priority = priority;
    }
#endif
    n->data = data;
    n->left = NULL;
    n->center = NULL;
    n->right = NULL;

    /* Replace */
    if ( NULL == *node ) {
        if ( bit < 0 ) {
            /* Same key */
            free(n);
            return -1;
        }
        if ( EXTRACT(mask, 0) ) {
            n->center = n;
        } else if ( EXTRACT(addr, 0) ) {
            n->right = n;
        } else {
            n->left = n;
        }
        *node = n;
        return 0;
    } else {
        for ( ; bit >= 0; bit-- ) {
            if ( EXTRACT((*node)->mask, bit) != EXTRACT(mask, bit) ) {
                /* Different mask */
                break;
            } else if ( EXTRACT((*node)->addr, bit) != EXTRACT(addr, bit)
                        && !EXTRACT((*node)->mask, bit) ) {
                /* Different addr */
                break;
            }
        }
        if ( bit < 0 ) {
            /* Same key - update if the new priority is higher */
            if ( priority > (*node)->priority ) {
                (*node)->priority = priority;
                (*node)->data = data;
#if PALMTRIE_PRIORITY_SKIP
                if ( priority > (*node)->max_priority ) {
                    (*node)->max_priority = priority;
                }
#endif
            }
            free(n);
            return 0;
        }
        n->bit = bit;
        if ( EXTRACT(mask, bit) ) {
            n->center = n;
        } else if ( EXTRACT(addr, bit) ) {
            n->right = n;
        } else {
            n->left = n;
        }
        if ( EXTRACT((*node)->mask, bit) ) {
            n->center = *node;
        } else if ( EXTRACT((*node)->addr, bit) ) {
            n->right = *node;
        } else {
            n->left = *node;
        }
        *node = n;

        return 0;
    }
}

/*
 * Add an internal node
 */
static int
_add_internal(struct palmtrie_tpt_node **node, addr_t addr, addr_t mask,
              int priority, void *data, int dbit)
{
    struct palmtrie_tpt_node *n;

    /* *node should not be NULL. */
    if ( NULL == *node ) {
        return -1;
    }

    /* Allocate a node */
    n = malloc(sizeof(struct palmtrie_tpt_node));
    if ( NULL == n ) {
        return -1;
    }
    n->bit = dbit;
    n->addr = addr;
    n->mask = mask;
    n->priority = priority;
#if PALMTRIE_PRIORITY_SKIP
    n->max_priority = (*node)->max_priority > priority
        ? (*node)->max_priority : priority;
#endif
    n->data = data;
    n->left = NULL;
    n->center = NULL;
    n->right = NULL;

    if ( EXTRACT(mask, dbit) ) {
        /* Center */
        n->center = n;
    } else if ( EXTRACT(addr, dbit) ) {
        /* Right */
        n->right = n;
    } else {
        /* Left */
        n->left = n;
    }
    if ( EXTRACT((*node)->mask, dbit) ) {
        /* Center */
        n->center = *node;
    } else if ( EXTRACT((*node)->addr, dbit) ) {
        /* Right */
        n->right = *node;
    } else {
        /* Left */
        n->left = *node;
    }
    if ( n->bit == (*node)->bit ) {
        fprintf(stderr, "Error in _add_internal()\n");
        return -1;
    }

    /* Replace */
    *node = n;

    return 0;
}

/*
 * Add an entry to the trie
 */
static int
_add(struct palmtrie_tpt_node **node, addr_t addr, addr_t mask, int priority,
     void *data, int cbit)
{
    int bit;
    struct palmtrie_tpt_node **next;

    if ( NULL == *node ) {
        /* Reaches at a null node */
        return _add_leaf(node, addr, mask, priority, data, 0);
    } else {
#if PALMTRIE_PRIORITY_SKIP
        if ( priority > (*node)->max_priority ) {
            (*node)->max_priority = priority;
        }
#endif

        /* Compare the prefix */
        bit = -1;
        for ( ; cbit > (*node)->bit; cbit-- ) {
            if ( EXTRACT((*node)->mask, cbit) != EXTRACT(mask, cbit) ) {
                /* Different mask */
                bit = cbit;
                break;
            } else if ( EXTRACT((*node)->addr, cbit) != EXTRACT(addr, cbit)
                        && !EXTRACT((*node)->mask, cbit) ) {
                /* Different addr */
                bit = cbit;
                break;
            }
        }
        if ( bit < 0 ) {
            /* Same prefix, then try to traverse */
            if ( EXTRACT(mask, (*node)->bit) ) {
                /* Center */
                next = &(*node)->center;
            } else if ( EXTRACT(addr, (*node)->bit) ) {
                /* Right */
                next = &(*node)->right;
            } else {
                /* Left */
                next = &(*node)->left;
            }

            if ( NULL == *next ) {
                /* Leaf */
                return _add_leaf(next, addr, mask, priority, data, cbit);
            } else if ( (*node)->bit <= (*next)->bit ) {
                /* Backtrack */
                return _add_leaf(next, addr, mask, priority, data,
                                 (*node)->bit);
            } else {
                /* Traverse to a descendent node */
                return _add(next, addr, mask, priority, data, (*node)->bit);
            }
        } else {
            /* Different prefix */
            return _add_internal(node, addr, mask, priority, data, bit);
        }
    }

    return -1;
}
int
palmtrie_tpt_add(struct palmtrie *palmtrie, addr_t addr, addr_t mask, int priority,
              void *data)
{
    return _add(&palmtrie->u.tpt.root, addr, mask, priority, data,
                PALMTRIE_ADDR_BITS - 1);
}

/*
 * Lookup an entry corresponding to the specified address
 */
static struct palmtrie_tpt_node *
_lookup(struct palmtrie_tpt_node *node, addr_t addr, int bit)
{
    struct palmtrie_tpt_node *lr;
    struct palmtrie_tpt_node *c;

    if ( NULL == node ) {
        return NULL;
    }
    if ( bit <= node->bit ) {
        /* Backtracked */
        if ( ADDR_MASK_CMP(addr, node->mask, node->addr, node->mask)
             && node->priority >= 0 ) {
            return node;
        } else {
            return NULL;
        }
    }

#if PALMTRIE_PRIORITY_SKIP
    struct palmtrie_tpt_node *n0;
    struct palmtrie_tpt_node *n1;

    c = node->center;
    if ( EXTRACT(addr, node->bit) ) {
        lr = node->right;
    } else {
        lr = node->left;
    }
    if ( NULL == c && NULL == lr ) {
        return NULL;
    } else if ( NULL == c ) {
        return _lookup(lr, addr, node->bit);
    } else if ( NULL == lr ) {
        return _lookup(c, addr, node->bit);
    } else {
        if ( c->max_priority > lr->max_priority ) {
            n0 = c;
            n1 = lr;
        } else {
            n0 = lr;
            n1 = c;
        }
        /* Search from higher priority */
        n0 = _lookup(n0, addr, node->bit);
        if ( NULL == n0 ) {
            return _lookup(n1, addr, node->bit);
        }
        if ( n0->priority > n1->max_priority ) {
            return n0;
        }
        n1 = _lookup(n1, addr, node->bit);
        if ( NULL == n1 ) {
            return n0;
        } else if ( n0->priority > n1->priority ) {
            return n0;
        } else {
            return n1;
        }
    }
#else
    /* Try to traverse center */
    c = _lookup(node->center, addr, node->bit);
    if ( EXTRACT(addr, node->bit) ) {
        /* Right */
        lr = _lookup(node->right, addr, node->bit);
    } else {
        /* Left */
        lr = _lookup(node->left, addr, node->bit);
    }

    if ( NULL == c && NULL == lr ) {
        return NULL;
    } else if ( NULL == c ) {
        return lr;
    } else if ( NULL == lr ) {
        return c;
    } else {
        if ( c->priority > lr->priority ) {
            return c;
        } else {
            /* Including same priority */
            return lr;
        }
    }
#endif
}
void *
palmtrie_tpt_lookup(struct palmtrie *palmtrie, addr_t addr)
{
    struct palmtrie_tpt_node *r;

    r = _lookup(palmtrie->u.tpt.root, addr, PALMTRIE_ADDR_BITS - 1);
    if ( NULL == r ) {
        return NULL;
    }

    return r->data;
}

/*
 * Recompute max_priority of a node from its own priority and descendants
 */
static void
_recompute_max_priority(struct palmtrie_tpt_node *node)
{
#if PALMTRIE_PRIORITY_SKIP
    int max = node->priority;
    if ( NULL != node->left ) {
        if ( node->left->max_priority > max ) {
            max = node->left->max_priority;
        }
    }
    if ( NULL != node->center ) {
        if ( node->center->max_priority > max ) {
            max = node->center->max_priority;
        }
    }
    if ( NULL != node->right ) {
        if ( node->right->max_priority > max ) {
            max = node->right->max_priority;
        }
    }
    node->max_priority = max;
#endif
}

/*
 * Recursively delete an entry matching the specified addr/mask.
 * Deletion marks the node as deleted (priority = -1) rather than
 * freeing it, to avoid dangling backtrack pointers from other nodes.
 */
static void *
_del(struct palmtrie_tpt_node **node, addr_t addr, addr_t mask, int cbit)
{
    struct palmtrie_tpt_node **next;
    void *data;

    if ( NULL == *node ) {
        return NULL;
    }

    /* Check if this is a terminal (backtrack) */
    if ( cbit <= (*node)->bit ) {
        if ( ADDR_CMP((*node)->addr, addr)
             && ADDR_CMP((*node)->mask, mask) ) {
            data = (*node)->data;
            (*node)->priority = -1;
            (*node)->data = NULL;
            _recompute_max_priority(*node);
            return data;
        }
        return NULL;
    }

    /* Follow the branch (same path as lookup) */
    if ( EXTRACT(mask, (*node)->bit) ) {
        next = &(*node)->center;
    } else if ( EXTRACT(addr, (*node)->bit) ) {
        next = &(*node)->right;
    } else {
        next = &(*node)->left;
    }

    if ( NULL == *next ) {
        /* No child - key not in trie */
        return NULL;
    } else if ( (*node)->bit <= (*next)->bit ) {
        /* Backtrack - *next is the terminal node */
        if ( *next == *node ) {
            /* Self-reference: key is at *node */
            if ( ADDR_CMP((*node)->addr, addr)
                 && ADDR_CMP((*node)->mask, mask) ) {
                data = (*node)->data;
                (*node)->priority = -1;
                (*node)->data = NULL;
                _recompute_max_priority(*node);
                return data;
            }
        } else {
            /* Backtrack to ancestor */
            if ( ADDR_CMP((*next)->addr, addr)
                 && ADDR_CMP((*next)->mask, mask) ) {
                data = (*next)->data;
                (*next)->priority = -1;
                (*next)->data = NULL;
                _recompute_max_priority(*next);
                return data;
            }
        }
        return NULL;
    }

    /* Recurse into descendant */
    data = _del(next, addr, mask, (*node)->bit);
    if ( NULL != data ) {
        _recompute_max_priority(*node);
    }
    return data;
}

/*
 * Delete an entry corresponding to the specified addr/mask
 */
int
palmtrie_tpt_delete(struct palmtrie *palmtrie, addr_t addr, addr_t mask)
{
    if ( NULL != _del(&palmtrie->u.tpt.root, addr, mask,
                      PALMTRIE_ADDR_BITS - 1) ) {
        return 0;
    }
    return -1;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
