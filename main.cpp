#include <iostream>
#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>

using namespace std;
using namespace chrono;

size_t detect_cache_size()
{
    const int repeats = 1000;
    size_t min_size = 4*1024;   // 4 KB
    size_t max_size = 256*1024; // 256 KB
    size_t best_size = min_size;
    double last_time = 0;

    for(size_t size = min_size; size <= max_size; size *= 2)
    {
        vector<int> data(size/sizeof(int), 1);
        auto start = steady_clock::now();

        volatile int sum = 0;
        for (int r = 0; r < repeats; ++r) {
            for (size_t i = 0; i < data.size(); i += 16)
                sum += data[i];
        }

        auto end = steady_clock::now();
        double elapsed_ms = duration_cast<microseconds>(end - start).count()/1000.0;

        if (size > min_size && elapsed_ms > last_time * 1.5) {
            best_size = size/2;
            break;
        }
        last_time = elapsed_ms;
    }
    return best_size;
}

size_t detect_cache_line()
{
    const int cache_size = 32*1024;
    vector<int> data(cache_size/sizeof(int), 1);
    const int stride_max = 1024;
    const int repeats = 100000;
    size_t best_stride = 4;
    double prev_time = 0;

    for (int stride = 4; stride <= stride_max; stride *= 2)
    {
        auto start = steady_clock::now();
        volatile int sum = 0;
        for (int r = 0; r < repeats; ++r)
            for (size_t i = 0; i < data.size(); i += stride)
                sum += data[i];

        auto end = steady_clock::now();
        double elapsed_ms = static_cast<double>(duration_cast<microseconds>(end - start).count())/1000.0;

        if (stride > 4 && elapsed_ms > prev_time * 1.5)
        {
            best_stride = stride / 2;
            break;
        }
        prev_time = elapsed_ms;
    }
    return best_stride * sizeof(int);
}

size_t detect_associativity()
{

    size_t cache_size = detect_cache_size();
    size_t line = detect_cache_line();
    size_t ways = 1;
    const int repeats = 100000;

    while (ways <= 32) {
        vector<size_t> offsets;
        for (size_t i = 0; i < ways; ++i)
            offsets.push_back(i * cache_size);

        vector<int> data((cache_size * (ways + 1)) / sizeof(int), 1);

        auto start = steady_clock::now();
        volatile int sum = 0;
        for (int r = 0; r < repeats; r++)
            for (size_t off : offsets)
                sum += data[off / sizeof(int)];

        auto end = steady_clock::now();
        double elapsed_ms = duration_cast<microseconds>(end - start).count()/1000.0;

        if (ways > 1 && elapsed_ms > 2 * ways) // почувствовать резкое замедление
            return ways - 1;
        ways++;
    }
    return ways;
}

int main() {
    cout << "Объем L1 cache: " << detect_cache_size()/1024 << " KB" << endl;
    return 0;
}
