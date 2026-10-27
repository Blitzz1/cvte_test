#pragma once
#include "app/types.hpp"
namespace avqm {
class Output {
  public:
    explicit Output(Application &app);
    ~Output();
    void run(int seconds = 0);
    void select_audio(int index);

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace avqm
