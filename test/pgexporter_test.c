/*
 * Copyright (C) 2025 The pgexporter community
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char project_directory[1024];

START_TEST(test_dummy)
{
    ck_assert_int_eq(1, 1);
}
END_TEST

START_TEST(test_pgexporter_basic)
{
    ck_assert_int_eq(2, 2);
}
END_TEST

Suite *
pgexporter_suite()
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("pgexporter");
    tc_core = tcase_create("Core");
    tcase_set_timeout(tc_core, 30);
    tcase_add_test(tc_core, test_dummy);
    tcase_add_test(tc_core, test_pgexporter_basic);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(int argc, char* argv[])
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    if (argc != 2)
    {
        return 1;
    }

    strncpy(project_directory, argv[1], sizeof(project_directory) - 1);

    s = pgexporter_suite();
    sr = srunner_create(s);
    srunner_run_all(sr, CK_VERBOSE);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? 0 : 1;
}