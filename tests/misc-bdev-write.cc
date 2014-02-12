#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <algorithm>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "stat.hh"
#include <osv/device.h>
#include <osv/bio.h>
#include <osv/prex.h>
#include <osv/mempool.hh>

#define MB (1024*1024)
#define KB (1024)

static std::chrono::high_resolution_clock s_clock;

std::atomic<int> bio_inflights(0);
std::atomic<long> bytes_written(0);

static void bio_done(struct bio* bio)
{
    auto err = bio->bio_flags & BIO_ERROR;
    bytes_written += bio->bio_bcount;
    memory::free_page(bio->bio_data);
    destroy_bio(bio);
    bio_inflights--;
    if (err) {
        printf("bio err!\n");
    }
}

int main(int argc, char const *argv[])
{
    struct device *dev;
    if (argc < 2) {
        printf("Usage: %s <dev-name>\n", argv[0]);
        return 1;
    }

    if (device_open(argv[1], DO_RDWR, &dev)) {
        printf("open failed\n");
        return 1;
    }

    long max_offset = 0;
    if (argc > 2) {
        max_offset = atol(argv[2]);
    }

    printf("bdev-write test offset limit: %ld byte(s)\n", max_offset);

    const std::chrono::seconds test_duration(10);
    const int buf_size = 4*KB;

    long total = 0;
    long offset = 0;

    auto test_start = s_clock.now();
    auto end_at = test_start + test_duration;

    stat_printer _stat_printer(bytes_written, [] (float bytes_per_second) {
        printf("%.3f Mb/s\n", (float)bytes_per_second / MB);
    }, 1000);

    while (s_clock.now() < end_at) {
        auto bio = alloc_bio();
        bio_inflights++;
        bio->bio_cmd = BIO_WRITE;
        bio->bio_dev = dev;
        bio->bio_data = memory::alloc_page();
        bio->bio_offset = offset;
        bio->bio_bcount = buf_size;
        bio->bio_caller1 = bio;
        bio->bio_done = bio_done;

        dev->driver->devops->strategy(bio);

        offset += buf_size;
        total += buf_size;

        if (max_offset != 0 && offset >= max_offset)
            offset = 0;
    }

    while (bio_inflights != 0) {
        usleep(2000);
    }

    auto test_end = s_clock.now();
    _stat_printer.stop();

    auto actual_test_duration = to_seconds(test_end - test_start);
    printf("Wrote %.3f MB in %.2f s = %.3f Mb/s\n", (double) total / MB, actual_test_duration,
            (double) total / MB / actual_test_duration);
    return 0;
}