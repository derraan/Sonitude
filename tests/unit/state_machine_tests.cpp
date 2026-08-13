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
  Require(out.zone_name == "front", "snapshot should preserve resolved zone");
  Require(out.confidence > 0.8F, "snapshot confidence should propagate from input");

  out = sm.update(in, 120'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Focused, "candidate->focused expected");

  in.has_track = false;
  in.confidence = 0.0F;
  in.speech_probability = 0.0F;
  out = sm.update(in, 130'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Held, "focused->held expected");

  out = sm.update(in, 260'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Releasing, "held->releasing expected");

  out = sm.update(in, 500'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Ambient, "releasing->ambient expected");
  Require(out.failsafe, "ambient should be failsafe");
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

void TestFocusedToReleasingOnAmbientCrossing()
{
  auto sm = BuildMachine();
  sonitude::control::ConversationInput in{};
  in.has_track = true;
  in.track = {0.0F, 0.0F};
  in.confidence = 0.9F;
  in.speech_probability = 0.9F;
  (void)sm.update(in, 10'000'000ULL);
  (void)sm.update(in, 150'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Focused, "expected focused state");

  in.track = {170.0F, 0.0F};
  (void)sm.update(in, 170'000'000ULL);
  Require(sm.state() == sonitude::control::ConversationState::Releasing,
          "focused source entering ambient zone should release focus");
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
        in.confidence = has_track ? 0.8F : 0.0F;
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
  TestAmbientPolicyBlocksCandidate();
  TestFocusedToReleasingOnAmbientCrossing();
  TestTransitionCoverageSanity();
}
