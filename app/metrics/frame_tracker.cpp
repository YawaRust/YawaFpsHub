#include "metrics/frame_tracker.h"

#include <algorithm>

namespace yawa {

FrameTracker::FrameTracker(std::uint64_t qpcFrequency)
    : m_qpcFrequency(qpcFrequency)
{
}

FrameTracker::Stream* FrameTracker::findStream(std::uint32_t processId, std::uint64_t swapChain)
{
    for (std::size_t index = 0; index < m_streamCount; ++index) {
        Stream& stream = m_streams[index];
        if (stream.processId == processId && stream.swapChain == swapChain) {
            return &stream;
        }
    }
    return nullptr;
}

FrameTracker::Stream* FrameTracker::claimStream(std::uint32_t processId, std::uint64_t swapChain)
{
    if (m_streamCount == kMaxStreams) {
        std::size_t oldest = 0;
        for (std::size_t index = 1; index < m_streamCount; ++index) {
            if (m_streams[index].lastQpc < m_streams[oldest].lastQpc) {
                oldest = index;
            }
        }
        m_streams[oldest] = Stream{};
        m_streams[oldest].processId = processId;
        m_streams[oldest].swapChain = swapChain;
        ++m_droppedStreams;
        m_lastTouched = oldest;
        return &m_streams[oldest];
    }

    Stream& stream = m_streams[m_streamCount];
    stream.processId = processId;
    stream.swapChain = swapChain;
    m_lastTouched = m_streamCount;
    ++m_streamCount;
    return &stream;
}

bool FrameTracker::isFresh(const Stream& stream, std::uint64_t newestQpc) const
{
    if (!stream.hasLast || m_qpcFrequency == 0) {
        return false;
    }
    const std::uint64_t limit = m_qpcFrequency * kStaleTimeoutSeconds;
    return newestQpc <= stream.lastQpc || newestQpc - stream.lastQpc <= limit;
}

bool FrameTracker::onPresent(const PresentSample& sample)
{
    if (m_qpcFrequency == 0) {
        return false;
    }

    Stream* stream = findStream(sample.processId, sample.swapChain);
    if (stream == nullptr) {
        stream = claimStream(sample.processId, sample.swapChain);
    }

    if (!stream->hasLast) {
        stream->lastQpc = sample.qpc;
        stream->bucketStartQpc = sample.qpc;
        stream->hasLast = true;
    } else if (sample.qpc >= stream->lastQpc) {
        const std::uint64_t bucketTicks = sample.qpc - stream->bucketStartQpc;
        if (bucketTicks >= m_qpcFrequency) {
            stream->ratePerSecond = stream->recentPresents * m_qpcFrequency / bucketTicks;
            stream->bucketStartQpc = sample.qpc;
            stream->recentPresents = 0;
        }

        const double elapsedMs = static_cast<double>(sample.qpc - stream->lastQpc) * 1000.0
            / static_cast<double>(m_qpcFrequency);

        // Долгая пауза — это смена окна или загрузка уровня, а не кадр на три секунды:
        // такой интервал не попадает ни в историю, ни в перцентили.
        if (elapsedMs > 0.0 && elapsedMs <= kMaxPlausibleFrameMs) {
            stream->frames.push(elapsedMs);
            if (stream->ratePerSecond == 0) {
                const auto instantRate = static_cast<std::uint64_t>(1000.0 / elapsedMs);
                stream->ratePerSecond = instantRate;
            }
        }
        stream->lastQpc = sample.qpc;
    } else {
        // Часы ETW могут отдать событие с меньшей меткой при смене адаптера: начинаем заново.
        stream->lastQpc = sample.qpc;
        stream->bucketStartQpc = sample.qpc;
        stream->recentPresents = 0;
        stream->ratePerSecond = 0;
    }

    ++stream->recentPresents;
    ++stream->totalPresents;
    if (sample.qpc > m_newestQpc) {
        m_newestQpc = sample.qpc;
    }
    return true;
}

void FrameTracker::reset()
{
    m_streams = {};
    m_streamCount = 0;
    m_lastTouched = 0;
    m_droppedStreams = 0;
    m_newestQpc = 0;
}

std::size_t FrameTracker::activeIndex() const
{
    std::size_t best = kMaxStreams;
    for (std::size_t index = 0; index < m_streamCount; ++index) {
        const Stream& stream = m_streams[index];
        if (!isFresh(stream, m_newestQpc)) {
            continue;
        }
        if (best == kMaxStreams || stream.ratePerSecond > m_streams[best].ratePerSecond) {
            best = index;
        } else if (stream.ratePerSecond == m_streams[best].ratePerSecond
            && stream.lastQpc > m_streams[best].lastQpc) {
            best = index;
        }
    }
    return best;
}

std::uint32_t FrameTracker::activeProcessId() const
{
    const std::size_t index = activeIndex();
    return index == kMaxStreams ? 0 : m_streams[index].processId;
}

std::uint64_t FrameTracker::activePresents() const
{
    const std::size_t index = activeIndex();
    return index == kMaxStreams ? 0 : m_streams[index].totalPresents;
}

std::uint64_t FrameTracker::activeSwapChain() const
{
    const std::size_t index = activeIndex();
    return index == kMaxStreams ? 0 : m_streams[index].swapChain;
}

bool FrameTracker::hasActiveStream() const
{
    return activeIndex() != kMaxStreams;
}

FpsSummary FrameTracker::summary(double targetFps) const
{
    const std::size_t index = activeIndex();
    if (index == kMaxStreams) {
        return FpsSummary{};
    }
    return FpsSummary::fromWindow(m_streams[index].frames, targetFps);
}

const FrameTimeWindow* FrameTracker::activeWindow() const
{
    const std::size_t index = activeIndex();
    return index == kMaxStreams ? nullptr : &m_streams[index].frames;
}

} // namespace yawa
