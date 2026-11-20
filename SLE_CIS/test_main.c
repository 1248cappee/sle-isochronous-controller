#include <stdio.h>

void test_control(void);
void test_data(void);
void test_radio(void);
void test_end_to_end(void);

int main(void)
{
    test_control();
    test_data();
    test_radio();
    test_end_to_end();
    puts("ALL TESTS PASSED");
    return 0;
}
