#include <array>
#include <cstdio>
#include <cstdlib>

// Exercise the production implementation, including its private external queue,
// without starting guest threads or loading a ROM. Only scheduling is stubbed.
#include "../lib/N64ModernRuntime/ultramodern/src/mesgqueue.cpp"

// CMake extracts the exact SI producer from events.cpp and rejects changes to
// its dependencies. This fixture supplies only its registered queue/message.
static struct {
    struct { PTR(OSMesgQueue) mq; OSMesg msg; } si;
} events_context{};
#ifndef RR64_SI_PRODUCER_INCLUDE
#define RR64_SI_PRODUCER_INCLUDE "rr64_si_message_producer.inc"
#endif
#include RR64_SI_PRODUCER_INCLUDE

namespace {
using Diagnostics = ultramodern::rr64_diagnostics::Event;
constexpr s32 MainQueue = static_cast<s32>(0x800B07F0u);
constexpr s32 ViQueue = static_cast<s32>(0x800AC8D0u);
constexpr s32 SiQueue = static_cast<s32>(0x800B0A50u);
constexpr s32 SpQueue = static_cast<s32>(0x800AC7E4u);
constexpr s32 DpQueue = static_cast<s32>(0x800AC80Cu);
constexpr s32 MessageBuffer = static_cast<s32>(0x80000200u);
constexpr s32 MessageOut = static_cast<s32>(0x80000300u);
constexpr s32 GuestThread = static_cast<s32>(0x80000600u);
alignas(64) std::array<uint8_t, 0xC0000> memory{};
std::array<unsigned long long, static_cast<unsigned int>(Diagnostics::Count)> counters{};
struct QueueEvent { unsigned int queue, outcome; unsigned long long age; };
std::array<QueueEvent, 32> queue_events{};
struct QueueTotals { unsigned int queue = 0; std::array<unsigned long long, 3> outcomes{}; };
std::array<QueueTotals, 5> queue_totals{};
std::size_t event_count = 0;
unsigned delivery_samples = 0, wait_samples = 0, yields = 0, scheduled = 0;
unsigned last_wait_queue = 0;
bool checked_running_queue = false;
bool passed = true;
s32 resume_queue = 0;

bool check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "[RR64-SCHEDULER-TEST] FAILED: %s\n", message);
        passed = false;
    }
    return condition;
}

void reset(s32 queue, int capacity) {
    external_messages.clear();
    memory.fill(0);
    counters.fill(0);
    queue_totals.fill({});
    event_count = delivery_samples = wait_samples = yields = scheduled = 0;
    checked_running_queue = false;
    resume_queue = 0;
    osCreateMesgQueue(memory.data(), queue, MessageBuffer, capacity);
}

void service(unsigned path) {
    if (path == 0) dequeue_external_messages(memory.data());
    else if (path == 1) ultramodern::wait_for_external_message(memory.data());
    else ultramodern::wait_for_external_message_timed(memory.data(), 1);
}

s32 receive_direct(s32 queue) {
    auto* rdram = memory.data();
    if (!check(do_recv(rdram, queue, MessageOut, false), "expected queued message")) return -1;
    return *TO_PTR(OSMesg, MessageOut);
}

unsigned long long observed_count(Diagnostics event) {
    return counters[static_cast<unsigned int>(event)];
}

unsigned long long queue_count(s32 queue, unsigned outcome) {
    for (const auto& totals : queue_totals) {
        if (totals.queue == static_cast<unsigned>(queue)) return totals.outcomes[outcome];
    }
    return 0;
}

void test_delivery_path(unsigned path) {
    reset(MainQueue, 2);
    ultramodern::enqueue_external_message(MainQueue, 11, false, false);
    ultramodern::enqueue_external_message(MainQueue, 22, false, false);
    service(path);
    if (path != 0) service(path);
    check(delivery_samples == 2 && event_count == 2, "initial deliveries recorded once");

    // Both types of full-queue outcome must retain their actual destination.
    ultramodern::enqueue_external_message(MainQueue, 33, false, false);
    external_messages.front().created_at -= std::chrono::milliseconds(50);
    service(path);
    check(external_messages.empty(), "unreliable message drops when guest queue is full");
    check(queue_events[event_count - 1].outcome == 2 &&
        queue_events[event_count - 1].queue == static_cast<unsigned>(MainQueue) &&
        queue_events[event_count - 1].age >= 50000000ull, "drop reports queue and original age");

    ultramodern::enqueue_external_message(MainQueue, 44, false, true);
    const auto original_time = rr64_diagnostics::Clock::now() - std::chrono::milliseconds(100);
    external_messages.front().created_at = original_time;
    service(path);
    check(external_messages.size() == 1 && external_messages.front().created_at == original_time,
        "retry preserves the original message timestamp");
    check(queue_events[event_count - 1].outcome == 1 &&
        queue_events[event_count - 1].age >= 100000000ull, "retry reports original age");
    const auto retry_age = queue_events[event_count - 1].age;
    check(receive_direct(MainQueue) == 11, "first queued message remains first");
    service(path);
    check(receive_direct(MainQueue) == 22 && receive_direct(MainQueue) == 44,
        "retry appends after existing messages");
    check(external_messages.empty() && queue_events[event_count - 1].outcome == 0 &&
        queue_events[event_count - 1].age >= retry_age, "eventual delivery includes retry residence");
    check(observed_count(Diagnostics::ExternalEnqueued) == 4 && observed_count(Diagnostics::ExternalDelivered) == 3 &&
        observed_count(Diagnostics::ExternalDropped) == 1 && observed_count(Diagnostics::ExternalRequeued) == 1 &&
        delivery_samples == 3 && event_count == 5, "new enqueue and outcome totals stay distinct");
}

void test_jam_and_destination() {
    reset(ViQueue, 3);
    ultramodern::enqueue_external_message(ViQueue, 1, false, false);
    ultramodern::enqueue_external_message(ViQueue, 2, false, false);
    ultramodern::enqueue_external_message(ViQueue, 3, true, false);
    dequeue_external_messages(memory.data());
    check(receive_direct(ViQueue) == 3 && receive_direct(ViQueue) == 1 && receive_direct(ViQueue) == 2,
        "jam still inserts before FIFO messages");
    check(event_count == 3 && queue_events[0].queue == static_cast<unsigned>(ViQueue) &&
        queue_events[2].queue == static_cast<unsigned>(ViQueue), "dynamic VI queue remains distinguishable");
}

void test_si_confirmation_surplus() {
    reset(SiQueue, 8);
    events_context.si.mq = SiQueue;
    std::deque<OSMesg> expected;
    OSMesg notification = 100;
    bool fifo_preserved = true;
    const auto start_read = [&]() {
        events_context.si.msg = notification++;
        ultramodern::send_si_message();
        if (expected.size() < 8) expected.push_back(events_context.si.msg);
        dequeue_external_messages(memory.data());
    };
    const auto consume_read = [&]() {
        const auto received = receive_direct(SiQueue);
        if (expected.empty()) {
            fifo_preserved = false;
        }
        else {
            fifo_preserved &= received == expected.front();
            expected.pop_front();
        }
    };

    start_read();
    // Original 73D80/73D88/73D90 confirmation sequence: each BAE0 consumes
    // and starts a read, while the intervening B4E0 starts one extra read.
    for (unsigned confirmation = 0; confirmation < 32; ++confirmation) {
        consume_read();
        start_read();
        start_read();
        consume_read();
        start_read();
    }
    // Fail promptly and specifically under the old SI policy; do not let its
    // intentionally retained surplus cascade into unrelated fixture failures.
    if (!check(external_messages.empty() && queue_count(SiQueue, 1) == 0,
        "SI surplus confirmations must drop on full instead of retaining retry backlog")) return;
    check(fifo_preserved && expected.size() == 8 && queue_count(SiQueue, 2) == 25,
        "32 confirmations preserve FIFO and drop only 25 excess SI notifications");

    for (unsigned pending = 0; pending < 8; ++pending) consume_read();
    start_read();
    consume_read();
    check(fifo_preserved && expected.empty(), "a new SI notification delivers after room becomes available");
    start_read();
    const auto drops_before_play = queue_count(SiQueue, 2);
    for (unsigned frame = 0; frame < 2000; ++frame) {
        consume_read();
        start_read();
    }
    check(fifo_preserved && expected.size() == 1 && external_messages.empty() &&
        queue_count(SiQueue, 1) == 0 && queue_count(SiQueue, 2) == drops_before_play,
        "continued normal reads neither lose notifications nor rebuild SI backlog");

    for (unsigned fill = 1; fill < 8; ++fill) start_read();
    osCreateMesgQueue(memory.data(), ViQueue, MessageBuffer + 0x40, 1);
    osCreateMesgQueue(memory.data(), SpQueue, MessageBuffer + 0x80, 1);
    osCreateMesgQueue(memory.data(), DpQueue, MessageBuffer + 0xC0, 1);
    ultramodern::enqueue_external_message(ViQueue, 10, false, false);
    ultramodern::enqueue_external_message(SpQueue, 20, false, true);
    ultramodern::enqueue_external_message(DpQueue, 30, false, true);
    dequeue_external_messages(memory.data());

    // A finite sweep drops the full SI/VI notifications while preserving the
    // reliable SP/DP completions for delivery when their queues have space.
    ultramodern::send_si_message();
    ultramodern::enqueue_external_message(ViQueue, 11, false, false);
    ultramodern::enqueue_external_message(SpQueue, 21, false, true);
    ultramodern::enqueue_external_message(DpQueue, 31, false, true);
    dequeue_external_messages(memory.data());
    check(external_messages.size() == 2 && queue_count(SiQueue, 1) == 0 &&
        queue_count(ViQueue, 2) == 1 && queue_count(SpQueue, 1) == 1 && queue_count(DpQueue, 1) == 1,
        "full SI cannot create retry work ahead of reliable SP/DP completions");
    check(receive_direct(ViQueue) == 10 && receive_direct(SpQueue) == 20 && receive_direct(DpQueue) == 30,
        "interleaved event queues retain their original contents");
    ultramodern::enqueue_external_message(ViQueue, 12, false, false);
    dequeue_external_messages(memory.data());
    check(external_messages.empty() && receive_direct(SpQueue) == 21 &&
        receive_direct(DpQueue) == 31 && receive_direct(ViQueue) == 12,
        "next finite sweep delivers pending SP/DP and fresh VI without SI interference");
    for (unsigned pending = 0; pending < 8; ++pending) consume_read();
    check(fifo_preserved, "SI overflow leaves every retained notification intact");
}

} // namespace

extern "C" void rr64_record_scheduler_stage(unsigned int stage, unsigned long long) {
    if (stage == static_cast<unsigned>(rr64_diagnostics::Stage::ExternalDelivery)) ++delivery_samples;
}
extern "C" void rr64_record_scheduler_event(unsigned int event, unsigned long long amount) {
    if (check(event < counters.size(), "valid event index")) counters[event] += amount;
}
extern "C" void rr64_record_external_queue_event(unsigned int queue, unsigned int outcome, unsigned long long age) {
    if (event_count < queue_events.size()) queue_events[event_count] = {queue, outcome, age};
    ++event_count;
    if (!check(outcome < 3, "valid per-queue outcome")) return;
    for (auto& totals : queue_totals) {
        if (totals.queue == 0 || totals.queue == queue) {
            totals.queue = queue;
            ++totals.outcomes[outcome];
            return;
        }
    }
    check(false, "bounded per-queue counter slots");
}
extern "C" void rr64_record_guest_queue_wait(unsigned int queue, unsigned long long) {
    check(checked_running_queue, "receive timing extends through final scheduler handoff");
    ++wait_samples;
    last_wait_queue = queue;
}

namespace ultramodern {
bool is_game_thread() { return true; }
PTR(OSThread) this_thread() { return GuestThread; }
bool thread_queue_empty(RDRAM_ARG PTR(PTR(OSThread)) queue) { return *TO_PTR(PTR(OSThread), queue) == 0; }
void thread_queue_insert(RDRAM_ARG PTR(PTR(OSThread)) queue, PTR(OSThread) thread) {
    check(*TO_PTR(PTR(OSThread), queue) == 0, "stub has only one blocked thread");
    *TO_PTR(PTR(OSThread), queue) = thread;
}
PTR(OSThread) thread_queue_pop(RDRAM_ARG PTR(PTR(OSThread)) queue) {
    const auto thread = *TO_PTR(PTR(OSThread), queue);
    *TO_PTR(PTR(OSThread), queue) = 0;
    return thread;
}
void schedule_running_thread(RDRAM_ARG PTR(OSThread) thread) {
    check(thread == GuestThread, "completion resumes the blocked guest");
    ++scheduled;
}
void check_running_queue(RDRAM_ARG1) { checked_running_queue = true; }
void run_next_thread_and_wait(RDRAM_ARG1) {
    if (!check(resume_queue != 0, "unexpected blocking operation")) std::abort();
    ++yields;
    const auto queue = resume_queue;
    resume_queue = 0;
    enqueue_external_message(queue, 77, false, true);
    dequeue_external_messages(rdram);
}
} // namespace ultramodern

int main() {
    // This fixture asserts the optional observations as well as queue behavior.
    // Enable them explicitly; normal player processes leave them disabled.
#ifdef _WIN32
    if (_putenv_s("RR64_DIAGNOSTICS", "1") != 0) return 2;
#else
    if (setenv("RR64_DIAGNOSTICS", "1", 1) != 0) return 2;
#endif
    for (unsigned path = 0; path < 3; ++path) test_delivery_path(path);
    test_jam_and_destination();
    for (const auto queue : {MainQueue, ViQueue}) {
        reset(queue, 2);
        resume_queue = queue;
        check(osRecvMesg(memory.data(), queue, MessageOut, OS_MESG_BLOCK) == 0,
            "blocking receive resumes on completion");
        check(yields == 1 && scheduled == 1 && wait_samples == 1 &&
            last_wait_queue == static_cast<unsigned>(queue), "blocking receive reports destination and handoff");
        checked_running_queue = false;
        ultramodern::enqueue_external_message(queue, 88, false, false);
        check(osRecvMesg(memory.data(), queue, MessageOut, OS_MESG_BLOCK) == 0 &&
            wait_samples == 2 && yields == 1 && delivery_samples == 2,
            "already pending external message drains inside measured receive");
        check(osRecvMesg(memory.data(), queue, MessageOut, OS_MESG_NOBLOCK) == -1 && wait_samples == 2,
            "nonblocking empty receive has no blocking-wait sample");
    }
    test_si_confirmation_surplus();
    if (passed) std::puts("[RR64-SCHEDULER-TEST] actual queue delivery, retry, ordering and observation passed.");
    return passed ? 0 : 1;
}
