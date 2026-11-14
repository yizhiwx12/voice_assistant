#include "playback_progress_clock.hpp"

#include <array>
#include <cassert>

int main()
{
    PlaybackProgressClock clock;
    std::array<int, 4> dma{};
    clock.reset();
    clock.bind(7);
    clock.onSent(&dma[0], true);  // 首次内容写入前的空环。
    assert(clock.snapshot().available && clock.snapshot().completed_samples == 0);
    clock.beforeWrite(&dma[0], 7, 240, true);
    clock.afterWrite(&dma[0]);
    clock.onSent(&dma[0], true);
    auto snapshot = clock.snapshot();
    assert(snapshot.available && snapshot.coherent && snapshot.serial == 7 && snapshot.completed_samples == 240);

    // DMA 完成与写入竞争时必须冻结时钟。
    // 不能通过推算的经过时间或只标记一部分的槽推进。
    clock.beforeWrite(&dma[1], 7, 480, true);
    clock.onSent(&dma[1], true);
    clock.afterWrite(&dma[1]);
    snapshot = clock.snapshot();
    assert(!snapshot.available && snapshot.completed_samples == 240);

    // 排空/欠载槽不能推进内容进度。
    clock.bind(8);
    clock.beforeWrite(&dma[2], 8, 240, false);
    clock.afterWrite(&dma[2]);
    clock.onSent(&dma[2], true);
    snapshot = clock.snapshot();
    assert(snapshot.available && snapshot.serial == 8 && snapshot.completed_samples == 0);

    // 相同 PCM（包括全零内容块）也有物理缓冲标签；
    // 该缓冲重复欠载回调不能重播内容。
    clock.beforeWrite(&dma[0], 8, 240, true);
    clock.afterWrite(&dma[0]);
    clock.onSent(&dma[0], true);
    clock.onSent(&dma[0], true);
    assert(clock.snapshot().completed_samples == 240);

    // pre-skip 让缓冲中只剩部分内容块。排空可以填满剩余空间，
    // 但必须保留精确有效内容端点。
    clock.beforeWrite(&dma[1], 8, 241, true);
    clock.afterWrite(&dma[1]);
    clock.beforeWrite(&dma[1], 8, 0, false);
    clock.afterWrite(&dma[1]);
    clock.onSent(&dma[1], true);
    assert(clock.snapshot().available && clock.snapshot().completed_samples == 241);

    // 静音内容不能变成“已播报”。只有排空后重新绑定
    // 才能从无法证明的边界恢复；陈旧序号不能推进。
    clock.beforeWrite(&dma[2], 8, 480, true);
    clock.afterWrite(&dma[2]);
    clock.onSent(&dma[2], false);
    assert(!clock.snapshot().available && clock.snapshot().completed_samples == 241);
    clock.bind(9);
    clock.beforeWrite(&dma[2], 8, 999, true);
    clock.afterWrite(&dma[2]);
    clock.onSent(&dma[2], true);
    assert(clock.snapshot().available && clock.snapshot().completed_samples == 0);

    // 完整 uint64 端点在原子低/高位发布后仍正确。
    clock.beforeWrite(&dma[3], 9, (uint64_t{1} << 32) + 123, true);
    clock.afterWrite(&dma[3]);
    clock.onSent(&dma[3], true);
    assert(clock.snapshot().completed_samples == (uint64_t{1} << 32) + 123);

    // 描述符几何不匹配时冻结游标，
    // 不能悄悄变成按实际经过时间的后备方案。
    int unknown_dma = 0;
    clock.onSent(&dma[0], true);
    clock.onSent(&dma[1], true);
    clock.onSent(&unknown_dma, true);
    assert(!clock.snapshot().available);
    clock.reset();
    assert(!clock.snapshot().available && clock.snapshot().serial == 0);
    return 0;
}
