/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Invoke unchanged existing host/disc query assertions; full HDD baseline EOF failures remain recorded. */
#define main t1085_full_hdd_main
#include "test_hdd_backing.c"
#undef main
int main(void)
{
    test_query_directory_lists_real_entries_one_per_call();
    test_query_directory_mask_semantics();
    test_query_directory_survives_deleting_the_returned_entry();
    test_query_directory_on_an_empty_and_a_created_directory();
    test_query_directory_reports_host_times_as_filetime();
    test_query_directory_refuses_what_is_not_measured();
    test_query_directory_refuses_each_unmeasured_argument_alone();
    test_query_directory_refuses_an_unreadable_mask();
    test_query_directory_restart_is_the_low_byte_only();
    test_query_directory_fixed_fields_times_and_allocation();
    test_query_directory_a_name_that_exactly_fits_is_returned();
    test_query_directory_restart_forgets_the_cursor_flag();
    test_query_directory_wildcard_corners();
    test_query_directory_case_only_different_names_are_both_listed();
    test_query_directory_reports_a_directory_size_of_zero_on_a_sized_filesystem();
    test_query_directory_on_a_disc_is_refused_not_answered_empty();
    printf("MU continuation unchanged host/disc query controls: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
