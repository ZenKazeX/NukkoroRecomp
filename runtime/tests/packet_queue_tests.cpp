#include "render/packet_queue.h"

#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <new>
#include <stdexcept>

namespace {
thread_local bool count_allocations = false;
thread_local std::size_t allocations = 0, allocated_bytes = 0;
thread_local void* tracked_payloads[2]{};
thread_local int track_next_payload = -1;
thread_local unsigned payloads_released = 0;
void release(void* pointer) noexcept {
    for (void*& tracked : tracked_payloads) {
        if (pointer && pointer == tracked) { tracked = nullptr; ++payloads_released; }
    }
    std::free(pointer);
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

// Count this thread's actual C++ allocations while exercising empty packets.
// Packet payloads are deliberately absent: this measures hand-over storage,
// not game geometry, GPU allocation or whole-game performance.
void* operator new(std::size_t size) {
    void* pointer = std::malloc(size ? size : 1);
    if (!pointer) throw std::bad_alloc();
    if (count_allocations) { ++allocations; allocated_bytes += size; }
    if (track_next_payload >= 0) { tracked_payloads[track_next_payload] = pointer; track_next_payload = -1; }
    return pointer;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { release(pointer); }
void operator delete[](void* pointer) noexcept { release(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { release(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { release(pointer); }

int main() {
    try {
        using namespace shadow::render;
        for (unsigned burst : {1u, 3u}) {
            PacketQueue queue(2);
            allocations = allocated_bytes = 0;
            count_allocations = true;
            bool matched = true;
            for (std::uint64_t cycle = 0; cycle < 20000; ++cycle) {
                for (unsigned i = 0; i < burst; ++i) {
                    FramePacket packet;
                    packet.tick = cycle * burst + i;
                    queue.push(std::move(packet));
                }
                FramePacket output;
                matched &= queue.pop_newest(&output, std::chrono::milliseconds(0)) &&
                           output.tick == (cycle + 1) * burst - 1;
            }
            count_allocations = false;
            const auto stats = queue.stats();
            require(matched && stats.pushed == 20000 * burst && stats.popped == 20000 &&
                    stats.dropped == 20000 * (burst - 1), "newest-frame delivery or drop accounting changed");
            std::printf("queue burst=%u cycles=20000 allocations=%zu bytes=%zu packet_bytes=%zu\n",
                        burst, allocations, allocated_bytes, sizeof(FramePacket));
            queue.close();
        }
        {
            PacketQueue queue(2);
            payloads_released = 0;
            for (unsigned i = 0; i < 2; ++i) {
                FramePacket packet;
                packet.tick = i + 1;
                // MSVC's large-vector alignment can put data() after the raw
                // allocation. Observe operator new's actual pointer so delete
                // is matched to ownership, not to an interior element address.
                track_next_payload = static_cast<int>(i);
                packet.immediate_indices.resize(4096);
                require(track_next_payload == -1 && tracked_payloads[i], "payload allocation was not observed");
                std::printf("payload %u data offset from allocation=%zu\n", i,
                    reinterpret_cast<std::uintptr_t>(packet.immediate_indices.data()) -
                    reinterpret_cast<std::uintptr_t>(tracked_payloads[i]));
                require(queue.push(std::move(packet)), "open queue refused a frame");
            }
            require(payloads_released == 0 && queue.stats().pending == 2, "queue lost owned payloads");
            queue.close();
            const auto stats = queue.stats();
            require(payloads_released == 2 && stats.pending == 0 && stats.pushed == 2 &&
                    stats.popped == 0 && stats.dropped == 0 && stats.discarded_on_close == 2,
                    "close must release both payloads and account for shutdown separately");
            queue.close();
            require(queue.stats().discarded_on_close == 2, "repeated close counted packets twice");
            FramePacket packet;
            packet.tick = 99;
            packet.immediate_indices = {1, 2, 3};
            const auto* owned = packet.immediate_indices.data();
            require(!queue.push(std::move(packet)) && packet.tick == 99 &&
                    packet.immediate_indices.data() == owned && packet.immediate_indices.size() == 3 &&
                    queue.stats().pushed == 2, "closed queue accepted or consumed a caller-owned packet");
            require(!queue.pop_newest(&packet, std::chrono::seconds(60)), "closed queue delivered cancelled work");
        }
        {
            PacketQueue queue(2);
            std::promise<void> entering;
            auto ready = entering.get_future();
            bool received = true;
            std::thread consumer([&] {
                FramePacket packet;
                entering.set_value();
                received = queue.pop_newest(&packet, std::chrono::seconds(60));
            });
            ready.wait();
            queue.close();
            consumer.join();
            require(!received, "close did not wake the empty consumer without work");
        }
        {
            PacketQueue queue(2);
            std::promise<void> started;
            auto ready = started.get_future();
            unsigned accepted = 0, refused = 0;
            std::thread producer([&] {
                for (unsigned i = 0; i < 10000; ++i) {
                    FramePacket packet;
                    packet.tick = i;
                    if (queue.push(std::move(packet))) ++accepted;
                    else ++refused;
                    if (i == 0) started.set_value();
                }
            });
            ready.wait();
            queue.close();
            producer.join();
            const auto stats = queue.stats();
            require(accepted + refused == 10000 && stats.pushed == accepted && stats.pending == 0 &&
                    stats.popped == 0 && stats.pushed == stats.dropped + stats.discarded_on_close,
                    "producer/close race lost packet ownership or accounting");
        }
        std::puts("PASS queue ownership: close frees payloads, rejects later pushes, wakes consumers and accounts for concurrent producers");
        return 0;
    } catch (const std::exception& error) {
        count_allocations = false;
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
