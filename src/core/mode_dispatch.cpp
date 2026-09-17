#include "core/mode_dispatch.h"

namespace mdk {

ModeDispatcher::ModeDispatcher()
    : primary_(mode::nativeBoot), sub_(mode::subModeNone) {}

void ModeDispatcher::setPrimary(PrimaryModeId id) {
  primary_ = id;
  sub_ = mode::subModeNone;
}

void ModeDispatcher::setSub(SubModeId id) { sub_ = id; }

void ModeDispatcher::requestQuit() { quit_ = true; }

void ModeDispatcher::on(PrimaryModeId id, Handler handler) {
  handlers_[id] = std::move(handler);
}

void ModeDispatcher::dispatch(const FrameContext& ctx) {
  const auto it = handlers_.find(primary_);
  if (it != handlers_.end() && it->second) {
    it->second(ctx);
  }
}

} // namespace mdk
