#include "render/gpu_timer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "render/present_trace.h"

namespace shadow::render {

namespace {

// QueryPerformanceCounter ticks to steady_clock nanoseconds, the way MSVC's steady_clock::now() converts them.
std::int64_t qpc_to_ns(std::int64_t counter, std::int64_t frequency) {
    const std::int64_t whole = (counter / frequency) * 1'000'000'000;
    const std::int64_t part = (counter % frequency) * 1'000'000'000 / frequency;
    return whole + part;
}

}  // namespace

std::string GpuTimer::requested_path() {
    const char* path = std::getenv("GEKKO_GPU_TRACE");
    return path != nullptr ? std::string(path) : std::string();
}

std::int64_t GpuTimer::now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool GpuTimer::create(Device& device, std::uint32_t slots, const std::string& path, std::string* error) {
    auto fail = [&](const char* message) {
        if (error != nullptr)
            *error = std::string("GEKKO_GPU_TRACE: ") + message;
        return false;
    };
    if (slots == 0)
        return fail("no frame slots");
    if (const char* draws = std::getenv("GEKKO_GPU_TRACE_DRAWS"); draws != nullptr && draws[0] != 0) {
        draws_path_ = draws;
        marks_per_slot_ = kDrawMarksPerSlot;
        if (const char* min_ms = std::getenv("GEKKO_GPU_TRACE_DRAWS_MIN_MS"); min_ms != nullptr && min_ms[0] != 0)
            draws_min_ms_ = std::atof(min_ms);
    }
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    desc.Count = slots * marks_per_slot_;
    if (FAILED(device.d3d()->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap_))))
        return fail("could not create the timestamp query heap");
    heap_->SetName(L"shadow.gpu_timer");
    std::string buffer_error;
    if (!create_buffer(device, std::uint64_t{desc.Count} * sizeof(std::uint64_t), D3D12_HEAP_TYPE_READBACK,
                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, L"shadow.gpu_timer.readback",
                       &readback_, &buffer_error))
        return fail(buffer_error.c_str());
    queue_ = device.direct().get();
    if (FAILED(queue_->GetTimestampFrequency(&gpu_frequency_)) || gpu_frequency_ == 0)
        return fail("the direct queue has no timestamp frequency");
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency_ = frequency.QuadPart;
    calibrate();
    pending_.assign(slots, Pending{});
    path_ = path;
    rows_.reserve(std::size_t{1} << 16);
    finished_ = false;
    return true;
}

void GpuTimer::destroy(Device& device) {
    if (readback_)
        retire(device, readback_);
    heap_.Reset();
    queue_ = nullptr;
    pending_.clear();
    open_ = false;
}

void GpuTimer::calibrate() {
    UINT64 gpu = 0, cpu = 0;
    if (SUCCEEDED(queue_->GetClockCalibration(&gpu, &cpu))) {
        calib_gpu_ = gpu;
        calib_ns_ = qpc_to_ns(static_cast<std::int64_t>(cpu), qpc_frequency_);
        // Every calibration pair, so the GPU-to-CPU clock mapping can be checked from the raw records (drift between
        // calibrations; docs/FRAME_TELEMETRY.md).
        present_trace::record(present_trace::Ev::GpuCalibration, gpu, gpu_frequency_,
                              static_cast<std::uint64_t>(calib_ns_));
    }
    last_calibration_ns_ = now_ns();
}

std::int64_t GpuTimer::gpu_to_ns(std::uint64_t gpu) const {
    const auto delta = static_cast<std::int64_t>(gpu - calib_gpu_);  // a mark before the calibration is negative
    const std::int64_t f = static_cast<std::int64_t>(gpu_frequency_);
    return calib_ns_ + (delta / f) * 1'000'000'000 + (delta % f) * 1'000'000'000 / f;
}

std::uint32_t GpuTimer::mark(ID3D12GraphicsCommandList* list) {
    Pending& p = pending_[slot_];
    if (p.marks >= marks_per_slot_) {
        p.overflow = true;
        return kNoMark;
    }
    const std::uint32_t index = p.marks++;
    list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * marks_per_slot_ + index);
    return index;
}

void GpuTimer::collect(std::uint32_t slot) {
    Pending& p = pending_[slot];
    if (!p.valid)
        return;
    p.valid = false;
    Row row{};
    row.frame = p.frame;
    row.tick = p.tick;
    row.game_ticks = p.game_ticks;
    row.to_image = p.to_image;
    row.slot = slot;
    row.cpu_begin = p.cpu_begin;
    row.cpu_submit = p.cpu_submit;
    row.cpu_present = p.cpu_present;
    row.passes = p.passes.size();
    row.barriers = p.barriers;
    row.overflow = p.overflow;
    row.counters = p.counters;
    if (p.marks != 0) {
        const SIZE_T first = SIZE_T{slot} * marks_per_slot_ * sizeof(std::uint64_t);
        const D3D12_RANGE range{first, first + SIZE_T{p.marks} * sizeof(std::uint64_t)};
        void* mapped = nullptr;
        if (SUCCEEDED(readback_.resource->Map(0, &range, &mapped))) {
            const std::uint64_t* t = static_cast<const std::uint64_t*>(mapped) + std::size_t{slot} * marks_per_slot_;
            if (p.begin_mark != kNoMark)
                row.gpu_begin = gpu_to_ns(t[p.begin_mark]);
            if (p.end_mark != kNoMark)
                row.gpu_end = gpu_to_ns(t[p.end_mark]);
            if (p.begin_mark != kNoMark && p.end_mark != kNoMark)
                row.gpu_ticks = t[p.end_mark] - t[p.begin_mark];
            const double row_ms = static_cast<double>(row.gpu_ticks) * 1000.0 / static_cast<double>(gpu_frequency_);
            if (!draws_path_.empty() && row_ms >= draws_min_ms_) {
                for (const DrawSpan& span : p.draws) {
                    if (span.begin == kNoMark || span.end == kNoMark || span.pass >= p.passes.size())
                        continue;
                    draw_rows_.push_back(
                        DrawRow{p.frame, p.tick, p.passes[span.pass].name, span.note, t[span.end] - t[span.begin]});
                }
            }
            row.pass_ticks.reserve(p.passes.size());
            for (const PassSpan& span : p.passes) {
                if (span.begin != kNoMark && span.end != kNoMark)
                    row.pass_ticks.emplace_back(span.name, t[span.end] - t[span.begin]);
            }
            const D3D12_RANGE nothing{0, 0};
            readback_.resource->Unmap(0, &nothing);
        }
    }
    rows_.push_back(std::move(row));
}

void GpuTimer::begin_frame(ID3D12GraphicsCommandList* list, std::uint32_t slot, std::uint64_t frame_number,
                           std::uint64_t packet_tick, std::uint64_t game_ticks, bool to_image) {
    const std::int64_t now = now_ns();
    if (now - last_calibration_ns_ >= 1'000'000'000)
        calibrate();
    collect(slot);
    slot_ = slot;
    open_ = true;
    Pending& p = pending_[slot];
    p.valid = false;
    p.to_image = to_image;
    p.frame = frame_number;
    p.tick = packet_tick;
    p.game_ticks = game_ticks;
    p.cpu_begin = now;
    p.cpu_submit = 0;
    p.cpu_present = 0;
    p.marks = 0;
    p.overflow = false;
    p.barriers = 0;
    p.passes.clear();
    p.draws.clear();
    p.counters = Counters{};
    p.end_mark = kNoMark;
    p.begin_mark = mark(list);
}

void GpuTimer::pass_begin(ID3D12GraphicsCommandList* list, const std::string& name) {
    if (!open_)
        return;
    auto found = name_ids_.find(name);
    if (found == name_ids_.end()) {
        found = name_ids_.emplace(name, static_cast<std::uint32_t>(names_.size())).first;
        names_.push_back(name);
    }
    PassSpan span;
    span.name = found->second;
    span.begin = mark(list);
    pending_[slot_].passes.push_back(span);
}

void GpuTimer::pass_end(ID3D12GraphicsCommandList* list) {
    if (!open_ || pending_[slot_].passes.empty())
        return;
    pending_[slot_].passes.back().end = mark(list);
}

void GpuTimer::draw_begin(ID3D12GraphicsCommandList* list) {
    Pending& p = pending_[slot_];
    if (p.passes.empty())
        return;
    DrawSpan span;
    span.pass = static_cast<std::uint32_t>(p.passes.size() - 1u);
    span.begin = mark(list);
    p.draws.push_back(span);
}

void GpuTimer::draw_end(ID3D12GraphicsCommandList* list, const DrawNote& note) {
    Pending& p = pending_[slot_];
    if (p.draws.empty() || p.draws.back().end != kNoMark)
        return;
    p.draws.back().end = mark(list);
    p.draws.back().note = note;
}

void GpuTimer::note_barriers(std::uint64_t count) {
    if (open_)
        pending_[slot_].barriers = count;
}

void GpuTimer::end_frame(ID3D12GraphicsCommandList* list, const Counters& counters) {
    if (!open_)
        return;
    Pending& p = pending_[slot_];
    p.end_mark = mark(list);
    p.counters = counters;
    if (p.marks != 0)
        list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * marks_per_slot_, p.marks,
                               readback_.resource.Get(),
                               std::uint64_t{slot_} * marks_per_slot_ * sizeof(std::uint64_t));
}

void GpuTimer::on_submitted() {
    if (!open_)
        return;
    Pending& p = pending_[slot_];
    p.cpu_submit = now_ns();
    p.valid = true;
}

void GpuTimer::on_presented() {
    if (!open_)
        return;
    pending_[slot_].cpu_present = now_ns();
    open_ = false;
}

void GpuTimer::finish() {
    if (finished_)
        return;
    finished_ = true;
    open_ = false;
    // Oldest frame first.
    std::vector<std::uint32_t> order;
    for (std::uint32_t s = 0; s < pending_.size(); ++s) {
        if (pending_[s].valid)
            order.push_back(s);
    }
    std::sort(order.begin(), order.end(),
              [&](std::uint32_t a, std::uint32_t b) { return pending_[a].frame < pending_[b].frame; });
    for (const std::uint32_t s : order)
        collect(s);
    std::FILE* out = std::fopen(path_.c_str(), "wb");
    if (out == nullptr)
        return;
    std::fprintf(out, "# shadow gpu trace 1, times in steady_clock ns, gpu timestamp frequency %llu Hz\n",
                 static_cast<unsigned long long>(gpu_frequency_));
    std::fputs("frame,tick,game_ticks,kind,slot,cpu_begin_ns,cpu_submit_ns,cpu_present_ns,gpu_begin_ns,gpu_end_ns,"
               "gpu_ms,draws,immediates,particles,passes,barriers,upload_bytes,upload_overflows,transient_desc,"
               "persistent_desc,vram_usage,vram_budget,mark_overflow,passes_detail\n",
               out);
    const double ms_per_tick = 1000.0 / static_cast<double>(gpu_frequency_);
    for (const Row& r : rows_) {
        const Counters& c = r.counters;
        std::fprintf(out, "%llu,%llu,%llu,%s,%u,%lld,%lld,%lld,%lld,%lld,%.4f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                          "%llu,%llu,%llu,%u,",
                     static_cast<unsigned long long>(r.frame), static_cast<unsigned long long>(r.tick),
                     static_cast<unsigned long long>(r.game_ticks), r.to_image ? "image" : "present", r.slot,
                     static_cast<long long>(r.cpu_begin), static_cast<long long>(r.cpu_submit),
                     static_cast<long long>(r.cpu_present), static_cast<long long>(r.gpu_begin),
                     static_cast<long long>(r.gpu_end), static_cast<double>(r.gpu_ticks) * ms_per_tick,
                     static_cast<unsigned long long>(c.draws), static_cast<unsigned long long>(c.immediates),
                     static_cast<unsigned long long>(c.particles), static_cast<unsigned long long>(r.passes),
                     static_cast<unsigned long long>(r.barriers), static_cast<unsigned long long>(c.upload_bytes),
                     static_cast<unsigned long long>(c.upload_overflows),
                     static_cast<unsigned long long>(c.transient_descriptors),
                     static_cast<unsigned long long>(c.persistent_descriptors),
                     static_cast<unsigned long long>(c.vram_usage), static_cast<unsigned long long>(c.vram_budget),
                     r.overflow ? 1u : 0u);
        for (std::size_t i = 0; i < r.pass_ticks.size(); ++i)
            std::fprintf(out, "%s%s:%.4f", i == 0 ? "" : "|", names_[r.pass_ticks[i].first].c_str(),
                         static_cast<double>(r.pass_ticks[i].second) * ms_per_tick);
        std::fputc('\n', out);
    }
    std::fclose(out);
    if (draws_path_.empty())
        return;
    std::FILE* draws = std::fopen(draws_path_.c_str(), "wb");
    if (draws == nullptr)
        return;
    std::fputs("frame,tick,pass,draw,kind,a,b,c,ms\n", draws);
    for (const DrawRow& r : draw_rows_)
        std::fprintf(draws, "%llu,%llu,%s,%u,%u,%u,%u,%u,%.4f\n", static_cast<unsigned long long>(r.frame),
                     static_cast<unsigned long long>(r.tick), names_[r.pass_name].c_str(), r.note.draw,
                     static_cast<unsigned>(r.note.kind), r.note.a, r.note.b, r.note.c,
                     static_cast<double>(r.ticks) * ms_per_tick);
    std::fclose(draws);
}

}  // namespace shadow::render
