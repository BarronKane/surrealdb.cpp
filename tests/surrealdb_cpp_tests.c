#include "unity.h"
#include "surrealdb.h"

void setUp(void) {}
void tearDown(void) {}

static void test_connect_memory(void) {
    sr_surreal_t* db = NULL;
    sr_string_t err = NULL;

    int result = sr_connect(&err, &db, "memory");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, result, "sr_connect should succeed");
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "db should not be null");

    if (result < 0 && err != NULL) {
        sr_free_string(err);
    }
    if (db != NULL) {
        sr_surreal_disconnect(db);
    }
}

static void test_object_basics(void) {
    sr_object_t obj = sr_object_new();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sr_object_len(&obj), "new object should be empty");
    sr_object_insert_str(&obj, "name", "alice");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sr_object_len(&obj), "object should have one key");
    sr_free_object(obj);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_connect_memory);
    RUN_TEST(test_object_basics);
    return UNITY_END();
}
