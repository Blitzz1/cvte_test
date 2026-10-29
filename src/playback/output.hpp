#pragma once
#include "app/types.hpp"
namespace avqm {
class Output {
  public:
    explicit Output(Application &app);
    ~Output();
    void run(int seconds = 0);
    void select_audio(int index);
    void select_audio_chain(const std::string &id);
    std::string selected_audio_id() const;
    void forget_chain(const std::shared_ptr<Chain> &chain);

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace avqm
