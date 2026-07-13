#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "services/camera_frame_pool.h"
#include "services/preview_frame_limiter.h"

namespace {

service::CameraFrame make_frame(uint16_t value) {
  service::CameraFrame frame;
  frame.width  = 2;
  frame.height = 2;
  frame.rgb565 = std::make_shared<std::vector<uint16_t>>(4, value);
  return frame;
}

void test_pool_reuses_only_released_buffers() {
  service::CameraFramePool pool(3);
  auto first  = pool.acquire(4);
  auto second = pool.acquire(4);
  auto third  = pool.acquire(4);
  assert(first && second && third);
  assert(!pool.acquire(4));

  auto* released_address = second.get();
  second.reset();
  auto reused = pool.acquire(8);
  assert(reused && reused.get() == released_address);
  assert(reused->size() == 8);
}

void test_frame_moves_without_copying_pixels() {
  auto frame       = make_frame(7);
  const auto* data = frame.data();
  service::CameraFrame service_frame = std::move(frame);
  service::CameraFrame view_frame    = std::move(service_frame);
  assert(view_frame.data() == data);
  assert((*view_frame.rgb565)[0] == 7);
}

void test_limiter_keeps_latest_frame_and_handles_wrap() {
  service::PreviewFrameLimiter limiter(33);
  service::CameraFrame output;

  limiter.push(make_frame(1));
  assert(limiter.take(std::numeric_limits<uint32_t>::max() - 10, output));
  assert((*output.rgb565)[0] == 1);

  limiter.push(make_frame(2));
  limiter.push(make_frame(3));
  assert(!limiter.take(5, output));
  assert(limiter.take(30, output));
  assert((*output.rgb565)[0] == 3);
  assert(limiter.presented_frames() == 2);
  assert(limiter.coalesced_frames() == 1);
}

}  // namespace

int main() {
  test_pool_reuses_only_released_buffers();
  test_frame_moves_without_copying_pixels();
  test_limiter_keeps_latest_frame_and_handles_wrap();
  return 0;
}
