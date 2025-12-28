// cache_probe.cpp
// Linux/macOS: L1 D-cache size, associativity (ways), way size, cache line size
// Timing: std::chrono::high_resolution_clock::now()
// Build:
//   Linux : g++   -O2 -march=native -std=c++17 cache_probe.cpp -o cache_probe
//   macOS : clang++ -O2 -march=native -std=c++17 cache_probe.cpp -o cache_probe

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

#include <unistd.h> // sysconf

#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

using high_resolution_clock = std::chrono::high_resolution_clock;

// *------------------------------------------------------------------------------------*
// |                                DATA UTILITIES                                      |
// *------------------------------------------------------------------------------------*
struct SizePoint {
    size_t bytes;
    double ns_per_access;
};

static double median(std::vector<double> &v) {
    if (v.empty()) return 0.0;
    auto n = v.size();
    std::nth_element(v.begin(), v.begin() + n / 2, v.end());
    double m = v[n / 2];
    if (n % 2 == 0) {
        auto it = std::max_element(v.begin(), v.begin() + n / 2);
        m = (m + *it) * 0.5;
    }
    return m;
}

static size_t detect_jump_bytes(const std::vector<SizePoint> &pts) {
    if (pts.size() < 10) return 0;

    const size_t baseN = std::min<size_t>(8, pts.size());
    std::vector<double> baseVals;
    baseVals.reserve(baseN);
    for (size_t i = 0; i < baseN; ++i) baseVals.push_back(pts[i].ns_per_access);
    const double base = median(baseVals);

    const double exceed_base_ratio = 1.35;
    const double local_jump_ratio = 1.18;
    const int confirm_points = 3;

    for (size_t i = 1; i + confirm_points < pts.size(); ++i) {
        const double cur = pts[i].ns_per_access;
        const double prev = pts[i - 1].ns_per_access;

        if (!(cur >= base * exceed_base_ratio && cur >= prev * local_jump_ratio))
            continue;

        bool ok = true;
        for (int k = 1; k <= confirm_points; ++k) {
            if (pts[i + k].ns_per_access < base * (exceed_base_ratio * 0.98)) {
                ok = false;
                break;
            }
        }
        if (ok) return pts[i - 1].bytes; // последняя "быстрая" точка ~ L1
    }
    return 0;
}

static size_t detect_jump_bytes_relaxed(const std::vector<SizePoint> &pts) {
    double baseline = pts.empty() ? 0.0 : pts.front().ns_per_access;
    std::size_t estimated = 0;

    for (std::size_t i = 1; i < pts.size(); ++i) {
        double prev = pts[i - 1].ns_per_access;
        double cur = pts[i].ns_per_access;


        if (cur > baseline * 1.30 && cur > prev * 1.15) {
            estimated = pts[i].bytes;
            break;
        }
    }

    if (estimated == 0) {
        estimated = std::max_element(pts.begin(), pts.end(), [](auto a, auto b) {
            return a.ns_per_access < b.ns_per_access;
        })->bytes;
    }

    return estimated;
}

static std::vector<size_t> make_sizes_grid() {
    std::vector<size_t> s;

    auto add_range = [&](size_t fromKB, size_t toKB, size_t stepKB) {
        for (size_t kb = fromKB; kb <= toKB; kb += stepKB)
            s.push_back(kb * 1024);
    };

    add_range(2, 32, 2);
    add_range(40, 128, 8);
    add_range(160, 512, 32);
    add_range(576, 1024, 64);

    return s;
}

static void build_random_cycle(std::vector<uint32_t> &next, uint32_t step = 16) {
    // step=16 for uint32_t => 64 bytes (типичный cache line).
    std::vector<uint32_t> idx;
    idx.reserve(next.size() / step + 1);
    for (uint32_t i = 0; i < (uint32_t) next.size(); i += step) idx.push_back(i);

    std::mt19937 rng(1234567);
    std::shuffle(idx.begin(), idx.end(), rng);

    for (size_t k = 0; k + 1 < idx.size(); ++k) next[idx[k]] = idx[k + 1];
    next[idx.back()] = idx.front();

    for (uint32_t i = 0; i < (uint32_t) next.size(); ++i)
        if (i % step != 0) next[i] = i;
}

static std::size_t align_up(std::size_t x, std::size_t align) {
    return (x + align - 1) & ~(align - 1);
}


// *------------------------------------------------------------------------------------*
// |                                 L1 size probe                                      |
// *------------------------------------------------------------------------------------*
static NOINLINE double measure_ns_per_access_random_cycle(
        size_t bytes,
        uint64_t total_accesses,
        int trials = 3
) {
    const size_t n = std::max<size_t>(bytes / sizeof(uint32_t), 1024);
    std::vector<uint32_t> next(n);
    build_random_cycle(next, 16);


    std::vector<double> results;
    results.reserve(trials);

    for (int t = 0; t < trials; ++t) {
        volatile uint32_t cur = 0;

        // warmup
        for (uint64_t i = 0; i < 200000; ++i) cur = next[cur];

        auto t0 = high_resolution_clock::now();
        for (uint64_t i = 0; i < total_accesses; ++i) cur = next[cur];
        auto t1 = high_resolution_clock::now();

#if defined(__GNUC__) || defined(__clang__)
        asm volatile("" : : "r"(cur) : "memory");
#endif

        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        results.push_back(ns / double(total_accesses));
    }
    return median(results);
}

static size_t detect_size_L1() {
    const auto sizes = make_sizes_grid();
    const uint64_t total_accesses = 1ULL * 1000ULL * 1000ULL;

    std::vector<SizePoint> pts;
    pts.reserve(sizes.size());

    std::cout << "\nL1 size probe:\n";
    std::cout << "Size(KB)\tns/access\n";
    for (size_t bytes: sizes) {
        double ns = measure_ns_per_access_random_cycle(bytes, total_accesses, 7);
        pts.push_back({bytes, ns});
        std::cout << (bytes / 1024) << "\t\t" << ns << "\n";
    }


    return detect_jump_bytes(pts);
}

// *------------------------------------------------------------------------------------*
// |                          L1 associativity (ways) probe                             |
// *------------------------------------------------------------------------------------*
static NOINLINE double measure_set_conflict_ns_per_access(
        size_t k_lines,
        size_t page_size,
        uint64_t accesses,
        int trials = 3
) {
    const size_t bytes = k_lines * page_size + page_size;
    void *raw = nullptr;
    if (posix_memalign(&raw, page_size, bytes) != 0) return 0.0;
    std::memset(raw, 1, bytes);


    std::vector<double> results;
    results.reserve(trials);

    for (int t = 0; t < trials; ++t) {
        std::vector<std::uintptr_t *> nodes;
        nodes.reserve(k_lines);

        for (int i = 0; i < k_lines; ++i) {
            auto p = (std::uintptr_t *) ((unsigned char *) raw + (size_t) i * page_size);
            nodes.push_back(p);
        }

        std::mt19937 rng(1000 + t);
        std::shuffle(nodes.begin(), nodes.end(), rng);

        for (int i = 0; i < k_lines - 1; ++i)
            *nodes[i] = (std::uintptr_t) nodes[i + 1];
        *nodes.back() = (std::uintptr_t) nodes.front();

        volatile std::uintptr_t cur = (std::uintptr_t) nodes.front();

        for (uint64_t i = 0; i < 200000; ++i) cur = *(std::uintptr_t *) cur;

        auto t0 = high_resolution_clock::now();
        for (uint64_t i = 0; i < accesses; ++i) cur = *(std::uintptr_t *) cur;
        auto t1 = high_resolution_clock::now();

#if defined(__GNUC__) || defined(__clang__)
        asm volatile("" : : "r"(cur) : "memory");
#endif

        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        results.push_back(ns / (double) accesses);
    }

    free(raw);
    return median(results);
}

static size_t detect_associativity_L1(size_t page_size) {
    const size_t k_min = 1;
    const size_t k_max = 32;
    const uint64_t total_accesses = 8ULL * 1000ULL * 1000ULL;


    std::vector<SizePoint> pts;
    pts.reserve(k_max - k_min + 1);

    std::cout << "\nAssociativity probe (same-set via page stride):\n";
    std::cout << "k_lines\t ns/access\n";

    for (size_t k = k_min; k <= k_max; ++k) {
        double ns = measure_set_conflict_ns_per_access(k, page_size, total_accesses, 9);
        pts.push_back({k, ns});
        std::cout << k << "\t " << ns << "\n";
    }

    return detect_jump_bytes(pts);
}

// *------------------------------------------------------------------------------------*
// |                                 L1 stride probe                                    |
// *------------------------------------------------------------------------------------*
struct Node {
    Node *next;
};

static NOINLINE double measure_stride(
        std::size_t mib,
        std::size_t page_size,
        std::size_t stride,
        std::uint64_t target_steps,
        int trials = 3
) {
    std::size_t bytes = mib * 1024ull * 1024ull;
    bytes = align_up(bytes, page_size);

    void *mem = nullptr;
    int rc = ::posix_memalign(&mem, page_size, bytes);
    if (rc != 0 || !mem) {
        std::cerr << "posix_memalign failed (rc=" << rc << ")\n";
        return 1;
    }
    std::memset(mem, 0, bytes);

    auto *base = reinterpret_cast<std::uint8_t *>(mem);

    // stride должен вмещать Node и быть кратным выравниванию указателя
    const std::size_t ptrAlign = alignof(Node);
    stride = std::max(stride, sizeof(Node));
    stride = align_up(stride, ptrAlign);

    std::size_t count = bytes / stride;
    if (count < 2) return std::numeric_limits<double>::infinity();

    // Создаём случайный порядок, чтобы снизить влияние аппаратного prefetch
    std::vector<std::size_t> idx(count);
    for (std::size_t i = 0; i < count; ++i) idx[i] = i;

    std::mt19937_64 rng(123456789ULL); // фиксированный seed для воспроизводимости
    std::shuffle(idx.begin(), idx.end(), rng);

    // Строим кольцо
    for (std::size_t i = 0; i < count; ++i) {
        auto cur_off = idx[i] * stride;
        auto next_off = idx[(i + 1) % count] * stride;
        Node *cur = reinterpret_cast<Node *>(base + cur_off);
        Node *next = reinterpret_cast<Node *>(base + next_off);
        cur->next = next;
    }

    Node *start = reinterpret_cast<Node *>(base + idx[0] * stride);

    // Сколько шагов делать, чтобы было достаточно "длинно"
    // (pointer chasing не векторизуется и почти не оптимизируется)
    std::uint64_t steps = target_steps;
    // Если узлов мало — гоняем по кругу много раз
    if (steps < count * 16) steps = static_cast<std::uint64_t>(count) * 16;

    // Небольшой прогрев
    volatile Node *p = start;
    for (std::uint64_t i = 0; i < std::min<std::uint64_t>(steps / 8, 2'000'000ULL); ++i) {
        p = p->next;
    }

    double best_ns_per = std::numeric_limits<double>::infinity();

    for (int t = 0; t < trials; ++t) {
        volatile Node *q = start;

        auto t0 = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < steps; ++i) {
            q = q->next;
        }
        auto t1 = std::chrono::steady_clock::now();

        // "используем" q, чтобы компилятор не выбросил цикл
        if (reinterpret_cast<std::uintptr_t>(q) == 0xdeadbeef) {
            std::cerr << "impossible\n";
        }

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        double ns_per = static_cast<double>(ns) / static_cast<double>(steps);
        best_ns_per = std::min(best_ns_per, ns_per);
    }

    free(base);

    return best_ns_per;
}


static size_t detect_stride_size_L1(size_t page_size) {
    std::size_t mib = 10;
    std::size_t max_stride = 1024;
    const std::uint64_t total_accesses = 16'000'00ULL;


    std::cout << "\n\nStride bytes\tns/access\n\n";


    std::vector<SizePoint> pts;

    // Stride: степени двойки от 8 до max_stride
    for (std::size_t stride = 8; stride <= max_stride; stride *= 2) {
        double ns = measure_stride(mib, page_size, stride, total_accesses);
        pts.push_back({stride, ns});
        std::cout << (stride) << "\t\t" << ns << "\n";
    }

    return detect_jump_bytes_relaxed(pts);
}

int main() {
    const auto page_size = size_t(sysconf(_SC_PAGESIZE));
    std::cout << "Page size: " << page_size << " bytes\n";

//     1) L1 size
    size_t l1_bytes = detect_size_L1();
    if (l1_bytes == 0) {
        std::cout << "\nL1 size jump not reliably detected in 2KB..1MB.\n";
    } else {
        std::cout << "\nEstimated L1 D-cache size: ~" << (l1_bytes / 1024) << " KB\n";
    }

    // 2) associativity (ways)
    size_t ways = detect_associativity_L1(page_size);
    if (ways == 0) {
        std::cout << "\nL1 associativity not reliably detected.\n";
    } else {
        std::cout << "\nEstimated L1 D-cache associativity: ~" << ways << "-way\n";
    }

    // 3) cache line size
    size_t line_bytes = detect_stride_size_L1(page_size);
    if (line_bytes == 0) {
        std::cout << "\nL1 cache line size not reliably detected.\n";
    } else {
        std::cout << "\nEstimated L1 D-cache line size: ~" << line_bytes << " B\n";
    }


    return 0;
}
