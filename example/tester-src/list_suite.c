#include "suites.h"
#include "../list.h"

TEST_START(test_list_new,
           "list_new() should return a pointer to an empty list on the heap") {

    uint64_t list_addr = test_call(list_new);
    test_assert_addr_not_equals(0, list_addr,
                                "list_new() should not return NULL");
    test_assert_malloced_block(list_addr, sizeof (list_t),
                               "list_new() should return a malloc()'d block "
                               "the size of a list_t)");

    list_t list;
    test_read_mem(list_addr, &list, sizeof list);
    test_assert_uint_equals(0, list.size,
                            "list_new() should return a list with size 0");
    test_assert(list.head == NULL, "list_new() should return a list with a "
                                    "NULL head pointer");
    test_assert_heap_state("list_new() should not leak memory",
                           {list_addr, "list_t struct"});
} TEST_END

TEST_START(test_list_new_oom,
           "list_new() should return NULL when out of memory") {

    tester_set_mallocs_until_fail(0);

    uint64_t list_addr = test_call(list_new);
    // TODO: make an equals macro to print both values
    test_assert_addr_equals(0, list_addr,
                            "list_new() should return NULL when malloc() "
                            "does");
    test_assert_heap_state("list_new() should not allocate memory"
                           /* no blocks */);
} TEST_END

TEST_START(test_list_push_empty,
           "list_push() should append an element to an empty list") {

    list_t list = { .size = 0, .head = NULL };
    uint64_t list_addr = test_make_heap_block(&list, sizeof list, NOT_FREEABLE);
    int data = 69;
    uint64_t data_addr = test_make_heap_block(&data, sizeof data, NOT_FREEABLE);

    int ret = (int) test_call(list_push, list_addr, data_addr);
    test_read_mem(list_addr, &list, sizeof list);

    test_assert_int_equals(1, ret, "list_push() should return 1 for success");
    test_assert_uint_equals(1, list.size,
                            "list_push() should increment the size of the list");
    test_assert_addr_not_equals(0, ADDR(list.head),
                                "list->head should not be NULL, since "
                                "list_push() should set a new head node.");
    test_assert_malloced_block(ADDR(list.head), sizeof (list_node_t),
                               "list_push() should set list->head to point to "
                               "a malloc()d list_node_t (the new node)");

    list_node_t head_node;
    test_read_mem(list.head, &head_node, sizeof head_node);

    test_assert_addr_equals(0, ADDR(head_node.next),
                            "list_push() should set list->head->next to NULL, "
                            "since the new node is the last node");
    test_assert_addr_equals(data_addr, ADDR(head_node.data),
                            "list_push() points list->head->data to the data "
                            "pointer passed in");

    test_assert_heap_state("list_push() should allocate only the new node",
                           {list_addr, "list_t struct passed in"},
                           {data_addr, "data passed in"},
                           {ADDR(list.head), "list_node_t struct created"});
} TEST_END

void add_list_suite(tester_t *tester) {
    tester_push(tester, test_list_new);
    tester_push(tester, test_list_new_oom);

    tester_push(tester, test_list_push_empty);
}
