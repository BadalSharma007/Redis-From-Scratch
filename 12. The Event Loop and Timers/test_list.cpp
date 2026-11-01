// learning/ch12/test_list.cpp      (NOT from the book: a small extra test)
//
// Checks the linked list, including the exact "move to the back"
// operation the server uses to renew a connection's timer.
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o test_list test_list.cpp
// Run:   ./test_list

#include <assert.h>
#include <stdio.h>
#include <vector>

#include "common.h"
#include "list.h"

using namespace std;

struct Item {
    int id;
    DList link;
};

// Read the ids in order, front to back.
static vector<int> order(DList *head) {
    vector<int> out;
    for (DList *n = head->next; n != head; n = n->next) {
        out.push_back(container_of(n, Item, link)->id);
    }
    return out;
}

// Same, but walking backwards: checks the prev links are consistent too.
static vector<int> order_backwards(DList *head) {
    vector<int> out;
    for (DList *n = head->prev; n != head; n = n->prev) {
        out.push_back(container_of(n, Item, link)->id);
    }
    return out;
}

int main() {
    DList head;
    dlist_init(&head);
    assert(dlist_empty(&head));

    Item a{1, {}}, b{2, {}}, c{3, {}};

    // inserting before the head appends at the back
    dlist_insert_before(&head, &a.link);
    dlist_insert_before(&head, &b.link);
    dlist_insert_before(&head, &c.link);
    assert(!dlist_empty(&head));
    assert((order(&head) == vector<int>{1, 2, 3}));
    assert((order_backwards(&head) == vector<int>{3, 2, 1}));
    printf("append order OK\n");

    // "renew the timer of the OLDEST" = detach the front, re-insert at the back
    dlist_detach(&a.link);
    dlist_insert_before(&head, &a.link);
    assert((order(&head) == vector<int>{2, 3, 1}));
    assert((order_backwards(&head) == vector<int>{1, 3, 2}));
    printf("move-front-to-back OK\n");

    // renew the one in the MIDDLE
    dlist_detach(&c.link);
    dlist_insert_before(&head, &c.link);
    assert((order(&head) == vector<int>{2, 1, 3}));
    printf("move-middle-to-back OK\n");

    // renewing the one already at the back changes nothing
    dlist_detach(&c.link);
    dlist_insert_before(&head, &c.link);
    assert((order(&head) == vector<int>{2, 1, 3}));
    printf("move-back-to-back OK\n");

    // remove everything: the list becomes empty again
    dlist_detach(&b.link);
    assert((order(&head) == vector<int>{1, 3}));
    dlist_detach(&a.link);
    dlist_detach(&c.link);
    assert(dlist_empty(&head));
    printf("removal and empty OK\n");

    printf("ALL TESTS PASSED\n");
    return 0;
}
