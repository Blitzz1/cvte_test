#pragma once
#include "app/types.hpp"
namespace avqm {
// Local RTSP/UDP relay: preserves media, exposes RTP observations and a separate datagram gate.
class RtspRelay {
  public:
    explicit RtspRelay(Application &app, int port = 8555);
    ~RtspRelay();
    void start();
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace avqm
