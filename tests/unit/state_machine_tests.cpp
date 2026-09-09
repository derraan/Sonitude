#include <stdexcept>
#include <string>
#include <vector>

#include "control/conversation_state_machine.hpp"

namespace
{
void Require(bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

sonitude::control::ConversationStateMachine BuildMachine()
{
  sonitude::app::StateMachineConfig cfg;
  cfg.activation_hold_ms = 100;
  cfg.confirmation_hold_ms = 50;
  cfg.release_hold_ms = 200;
  cfg.hold_direction_ms = 120;
  cfg.zone_direction_stability_deg = 15.0F;
  std::vector<sonitude::app::ZoneConfig> zones = {
      {"front", -45.0F, 45.0F, sonitude::app::ZonePolicy::Focus},
      {"assist", 45.0F, 100.0F, sonitude::app::ZonePolicy::Assist},
      {"rear", 100.0F, 260.0F, sonitude::app::ZonePolicy::Ambient},
  };
  return sonitude::control::ConversationStateMachine(cfg, 0.25F, sonitude::control::ZoneMap(zones));
}

void TestTransitionPathAndHysteresis()
{
  auto sm = BuildMachine();

  sonitude::control::ConversationInput in{};
  in.has_track = true;
  in.track = {20.0F, 0.0F};
  in.confidence = 0.85F;
  in.speech_probability = 0.9F;

  auto out = sm.update(in, 10'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Candidate, "ambient->candidate expected");
  Require(out.failsafe == false, "candidate should be non-failsafe");
  Require(out.zone_id == 0, "the first configured zone should use index zero");
  Require(out.confidence == in.confidence, "source confidence should propagate");

  out = sm.update(in, 120'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Focused, "candidate->focused expected");

  in.has_track = false;
  in.speech_probability = 0.0F;
  out = sm.update(in, 130'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Held, "focused->held expected");

  out = sm.update(in, 260'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Releasing, "held->releasing expected");

  out = sm.update(in, 500'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Ambient, "releasing->ambient expected");
  Require(out.failsafe, "ambient should be failsafe");
}

void FocusMachine(sonitude::control::ConversationStateMachine& sm,
                  sonitude::control::ConversationInput& input)
{
  input.has_track = true;
  input.track = {0.0F, 0.0F};
  input.confidence = 0.9F;
  input.speech_probability = 0.9F;
  (void)sm.update(input, 10'000'000ULL);
  (void)sm.update(input, 120'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Focused,
          "test precondition must reach Focused");
}

void TestAmbientNeverReacquiresFocus()
{
  auto sm = BuildMachine();
  sonitude::control::ConversationInput input{};
  FocusMachine(sm, input);

  input.track = {170.0F, 0.0F};
  (void)sm.update(input, 130'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Releasing,
          "entering an Ambient zone while Focused must begin release");

  (void)sm.update(input, 190'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Releasing,
          "persistent Ambient speech beyond confirmation hold must not reacquire focus");

  (void)sm.update(input, 340'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Ambient,
          "persistent Ambient input must eventually complete release");

  (void)sm.update(input, 500'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Ambient,
          "Ambient speech must not cause an unauthorized transition back to Focused");
}

void TestHeldRejectsAmbientButAssistReacquires()
{
  auto sm = BuildMachine();
  sonitude::control::ConversationInput input{};
  FocusMachine(sm, input);

  input.has_track = false;
  input.confidence = 0.0F;
  input.speech_probability = 0.0F;
  (void)sm.update(input, 130'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Held,
          "missing focus input must enter Held");

  input.has_track = true;
  input.track = {170.0F, 0.0F};
  input.confidence = 0.9F;
  input.speech_probability = 0.9F;
  (void)sm.update(input, 150'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Held,
          "Ambient input while Held must not reacquire focus");

  input.track = {70.0F, 0.0F};
  (void)sm.update(input, 160'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Focused,
          "Assist-zone speech must reacquire focus from Held");
}

void TestAssistReacquiresFromReleasing()
{
  auto sm = BuildMachine();
  sonitude::control::ConversationInput input{};
  FocusMachine(sm, input);

  input.track = {80.0F, 0.0F};
  (void)sm.update(input, 130'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Releasing,
          "a direction change outside stability must begin release");
  (void)sm.update(input, 190'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Focused,
          "Assist-zone speech may reacquire after confirmation hold");
}

void TestAmbientPolicyBlocksCandidate()
{
  auto sm = BuildMachine();
  sonitude::control::ConversationInput in{};
  in.has_track = true;
  in.track = {170.0F, 0.0F};
  in.confidence = 0.9F;
  in.speech_probability = 0.9F;
  (void)sm.update(in, 10'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Ambient,
          "ambient zone should block ambient->candidate transition");
}

void TestTransitionCoverageSanity()
{
  auto sm = BuildMachine();
  const std::vector<bool> has_track_cases = {false, true};
  const std::vector<float> speech_cases = {0.2F, 0.9F};
  std::uint64_t now = 0;
  for (int iter = 0; iter < 20; ++iter)
  {
    for (const bool has_track : has_track_cases)
    {
      for (const float speech : speech_cases)
      {
        sonitude::control::ConversationInput in{};
        in.has_track = has_track;
        in.track = {has_track ? 30.0F : 0.0F, 0.0F};
        in.speech_probability = speech;
        now += 110'000'000ULL;
        (void)sm.update(in, now);
      }
    }
  }
  std::uint64_t total_transitions = 0;
  for (std::size_t from = 0; from < static_cast<std::size_t>(sonitude::control::ConversationState::kCount);
       ++from)
  {
    for (std::size_t to = 0;
         to < static_cast<std::size_t>(sonitude::control::ConversationState::kCount);
         ++to)
    {
      total_transitions += sm.transitionCount(
          static_cast<sonitude::control::ConversationState>(from),
          static_cast<sonitude::control::ConversationState>(to));
    }
  }
  Require(total_transitions > 0, "expected transition coverage entries");
}
}  // namespace

void RunStateMachineTests()
{
  TestTransitionPathAndHysteresis();
  TestAmbientNeverReacquiresFocus();
  TestHeldRejectsAmbientButAssistReacquires();
  TestAssistReacquiresFromReleasing();
  TestAmbientPolicyBlocksCandidate();
  TestTransitionCoverageSanity();
}
