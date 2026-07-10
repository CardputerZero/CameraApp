#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>

#include "screens/screen_manager.h"
#include "services/camera_backend_utils.h"
#include "services/video_recorder.h"
#include "viewmodels/camera_viewmodel.h"

namespace {

struct LifecycleCounts {
  int enters{0};
  int exits{0};
};

class CountingViewModel final : public viewmodel::BaseViewModel {
 public:
  explicit CountingViewModel(std::shared_ptr<LifecycleCounts> counts)
      : counts_(std::move(counts)) {}

  void on_enter() override { ++counts_->enters; }
  void on_exit() override { ++counts_->exits; }

 private:
  std::shared_ptr<LifecycleCounts> counts_;
};

std::shared_ptr<screen::Screen> make_screen(const std::shared_ptr<LifecycleCounts>& counts) {
  return std::make_shared<screen::Screen>(
      nullptr,
      std::make_unique<view::BaseView>(nullptr),
      std::make_shared<CountingViewModel>(counts));
}

void test_screen_lifecycle() {
  auto first  = std::make_shared<LifecycleCounts>();
  auto second = std::make_shared<LifecycleCounts>();
  screen::ScreenManager manager(nullptr);
  manager.register_screen("first", [first](lv_obj_t*) { return make_screen(first); });
  manager.register_screen("second", [second](lv_obj_t*) { return make_screen(second); });
  manager.register_screen("broken", [](lv_obj_t*) { return std::shared_ptr<screen::Screen>{}; });

  assert(manager.push_screen("first"));
  assert(first->enters == 1 && first->exits == 0);
  assert(!manager.push_screen("missing"));
  assert(!manager.replace_screen("broken"));
  assert(first->enters == 1 && first->exits == 0);

  assert(manager.replace_screen("second"));
  assert(first->enters == 1 && first->exits == 1);
  assert(second->enters == 1 && second->exits == 0);

  manager.clear();
  assert(second->enters == 1 && second->exits == 1);
}

void test_unique_media_paths() {
  const auto dir = std::filesystem::temp_directory_path() / "camera_app_core_tests";
  std::filesystem::remove_all(dir);

  std::set<std::string> paths;
  for (int i = 0; i < 64; ++i) {
    paths.insert(service::camera_backend::make_unique_media_path(
        dir.string(), "TEST", "jpg"));
  }
  assert(paths.size() == 64);
  assert(std::filesystem::is_directory(dir));

  const auto existing = dir / "existing.avi";
  {
    std::ofstream file(existing, std::ios::binary);
    file << "keep";
  }
  service::camera_backend::MjpegAviWriter writer;
  assert(!writer.open(existing.string(), 16, 16, 10));
  std::ifstream file(existing, std::ios::binary);
  assert(std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>()) == "keep");

  std::filesystem::remove_all(dir);
}

void test_rejected_capture_has_no_feedback() {
  viewmodel::CameraViewModel viewmodel;
  assert(!viewmodel.handle_action(app::AppAction::Capture));
  assert(!viewmodel.consume_capture_feedback());

  auto services    = std::make_shared<service::AppServices>();
  services->camera = std::make_shared<service::CameraService>();
  viewmodel::CameraViewModel unavailable_camera(services);
  assert(!unavailable_camera.handle_action(app::AppAction::Capture));
  assert(!unavailable_camera.consume_capture_feedback());
}

}  // namespace

int main() {
  test_screen_lifecycle();
  test_unique_media_paths();
  test_rejected_capture_has_no_feedback();
  return 0;
}
