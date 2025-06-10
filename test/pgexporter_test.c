/*
 * Dummy test file - just to make CMake happy
 */

#include <check.h>
#include <stdio.h>

START_TEST(test_dummy)
{
    printf("Dummy test - infrastructure setup works!\n");
    ck_assert_int_eq(1, 1);
}
END_TEST

Suite *
pgexporter_suite()
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("pgexporter");
    tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_dummy);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = pgexporter_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_VERBOSE);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}