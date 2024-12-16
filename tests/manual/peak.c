/*
    SPDX-FileCopyrightText: 2017 Milian Wolff <mail@milianw.de>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include <stdlib.h>

char* allocate_something(int size)
{
    return malloc((size_t)size);
}

char* foo()
{
    return allocate_something(100);
}

char* bar()
{
    return allocate_something(25);
}

int main()
{
    char* f1 = foo();
    char* b2 = bar();
    free(f1);
    char* b3 = bar();
    char* b4 = bar();
    free(b2);
    free(b3);
    free(b4);
    char* f2 = foo();
    free(f2);
    return 0;
}
