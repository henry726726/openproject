/*
    SPDX-FileCopyrightText: 2014-2017 Milian Wolff <mail@milianw.de>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include <future>
#include <thread>
#include <vector>

using namespace std;

const int ALLOCS_PER_THREAD = 1000;

int** alloc()
{
    int** block = new int*[ALLOCS_PER_THREAD];
    for (int i = 0; i < ALLOCS_PER_THREAD; ++i) {
        block[i] = new int;
    }
    return block;
}

void dealloc(future<int**>&& f)
{
    int** block = f.get();
    for (int i = 0; i < ALLOCS_PER_THREAD; ++i) {
        delete block[i];
    }
    delete[] block;
}

int main()
{
    vector<future<void>> futures;
    futures.reserve(100 * 4);
    for (int i = 0; i < 100; ++i) {
        auto f1 = async(launch::async, alloc);
        auto f2 = async(launch::async, alloc);
        auto f3 = async(launch::async, alloc);
        auto f4 = async(launch::async, alloc);
        futures.emplace_back(async(launch::async, dealloc, std::move(f1)));
        futures.emplace_back(async(launch::async, dealloc, std::move(f2)));
        futures.emplace_back(async(launch::async, dealloc, std::move(f3)));
        futures.emplace_back(async(launch::async, dealloc, std::move(f4)));
    }
    return 0;
}
