#include "AiPetModel.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <set>

using namespace serialctl::pet;
int main() {
    Model model;
    model.Page(true, 100);
    assert(model.OpeningDue() == 4100);
    model.Page(true, 1000); // Layout/focus changes are not page entries.
    assert(model.OpeningDue() == 4100);
    assert(!model.At(4099).manual);
    assert(model.At(4100).opening);
    assert(model.CurrentAction() == Action::Boxing);
    std::set<unsigned> hops;
    unsigned airborneIntervals = 0;
    bool airborne = false;
    for (unsigned ms = 0; ms < 8000; ms += 10) {
        const auto s = model.At(4100 + ms);
        assert(s.pose < 36 && s.pose != 16);
        if (s.hop)
            hops.insert(s.hop);
        bool next = s.lift > .001;
        if (next && !airborne)
            ++airborneIntervals;
        airborne = next;
        if (s.punching)
            assert(ms >= 3900 && s.hop == 0 && s.lift == 0);
    }
    assert(hops == std::set<unsigned>({1, 2, 3}) && airborneIntervals == 3);
    assert(model.At(4100 + 1050).x > 0);
    assert(model.At(4100 + 2250).x < 0);
    assert(model.At(4100 + 3000).x < 0);
    assert(model.At(4100 + 3300).x == 0);
    assert(!model.At(12100).manual);

    model.Page(false, 13000);
    model.Page(true, 14000);
    assert(model.OpeningDue() == 18000);
    model.Page(false, 17000);
    assert(!model.At(19000).manual && model.OpeningDue() == 0);
    model.Page(true, 20000);
    model.Play(Action::Wave, 21000);
    assert(model.OpeningDue() == 0);
    model.SetActivity(Activity::Working, 21500);
    assert(model.At(21600).manual);
    assert(model.At(23000).pose == PoseFor(Character::Whale, Action::Work) && !model.At(23000).manual);

    Model paused;
    paused.Page(true, 0);
    paused.At(1000, false);
    paused.At(10000, false);
    paused.At(11000, true);
    assert(paused.OpeningDue() == 15000);
    assert(!paused.At(14999).manual && paused.At(15000).opening);
    const auto before = paused.At(15150).pose;
    paused.At(15150, false);
    const auto resumed = paused.At(20000, true);
    assert(resumed.pose == before);

    paused.At(20100, false);
    paused.Play(Action::Wave, 25000);
    assert(paused.At(25200).pose == 30); // New manual action must not inherit the old pause.

    Model priorActivity;
    priorActivity.SetActivity(Activity::Failed, 1);
    priorActivity.Page(true, 100);
    assert(priorActivity.At(4100).opening);
    assert(priorActivity.CurrentAction() == Action::Boxing);

    Model changedWhilePaused;
    changedWhilePaused.Page(true, 0);
    changedWhilePaused.At(1000, false);
    changedWhilePaused.SetActivity(Activity::Done, 3000);
    assert(changedWhilePaused.At(5000, true).pose == PoseFor(Character::Whale, Action::Cheer));
    assert(changedWhilePaused.At(6000, true).pose == PoseFor(Character::Whale, Action::Idle));
    Settings settings;
    settings.opening = false;
    model.Configure(settings, 24000);
    model.Page(false, 24000);
    model.Page(true, 25000);
    assert(!model.At(35000).manual);
    model.Play(Action::Boxing, 36000);
    assert(model.At(36001).manual);
    model.SetActivity(Activity::Failed, 36100);
    model.Stop(36500);
    assert(model.At(36501).pose == PoseFor(Character::Whale, Action::Concern));
    model.SetActivity(Activity::Stopped, 37000);
    assert(model.At(37001).pose == PoseFor(Character::Whale, Action::Settle));
    model.SetActivity(Activity::Done, 38000);
    assert(model.At(38100).pose == PoseFor(Character::Whale, Action::Cheer));
    assert(model.At(40000).pose == PoseFor(Character::Whale, Action::Idle));
    model.SetActivity(Activity::Done, 40100);
    assert(model.At(40200).pose == PoseFor(Character::Whale, Action::Idle));

    settings.enabled = false;
    model.Configure(settings, 41000);
    model.Play(Action::Boxing, 42000);
    assert(!model.Active() && !model.At(42000).manual);
    settings.enabled = true;
    settings.follow = false;
    settings.size = 111;
    settings.x = -200;
    settings.y = 3000;
    model.Configure(settings, 43000);
    assert(model.Preferences().size == 96);
    assert(model.Preferences().x == 0 && model.Preferences().y == 1000);
    model.SetActivity(Activity::Working, 43000);
    assert(model.At(44000).pose == PoseFor(Character::Whale, Action::Idle));
    std::set<unsigned> spriteSlots;
    for (const auto character : {Character::Whale, Character::Lulu, Character::Cat}) {
        Settings preferences;
        preferences.character = character;
        preferences.opening = false;
        Model companion;
        companion.Configure(preferences, 1);
        companion.Page(true, 1);
        const auto actions = ActionsFor(character);
        assert(actions.count == (character == Character::Whale ? 15u : 12u));
        for (unsigned i = 0; i < actions.count; ++i) {
            const auto action = actions.items[i].action;
            assert(spriteSlots.insert(PoseFor(character, action)).second);
            companion.Play(action, 2);
            assert(companion.At(80).manual);
            if (character != Character::Whale)
                assert(companion.At(80).pose == PoseFor(character, action));
        }
        companion.Stop(100);
        unsigned time = 101;
        for (const auto &state : {std::pair{Activity::Connecting, Action::Listen},
                                  std::pair{Activity::Thinking, Action::Think},
                                  std::pair{Activity::Working, Action::Work},
                                  std::pair{Activity::Replying, Action::Reply},
                                  std::pair{Activity::Done, Action::Cheer},
                                  std::pair{Activity::Failed, Action::Concern},
                                  std::pair{Activity::Stopped, Action::Settle}}) {
            companion.SetActivity(state.first, time);
            assert(companion.At(time + 10).pose == PoseFor(character, state.second));
            time += 100;
        }
        companion.SetActivity(Activity::Idle, time);
        assert(companion.At(time + 45000).pose == PoseFor(character, Action::Sleep));
    }
    assert(spriteSlots.size() == 39);
    RoamingClock departure;
    departure.Step(1000, true);
    RoamingClock::Position visible;
    for (unsigned time = 1080; time <= 1400; time += 80)
        visible = departure.Step(time, true);
    assert(std::lround(visible.x) > 0 && std::lround(visible.y) > 0);
    RoamingClock roam;
    assert(!roam.Step(1000, true).moving);
    const auto first = roam.Step(1040, true);
    assert(first.moving && first.x > 0 && first.y != 0);
    const auto pausedPosition = roam.Step(100000, false);
    assert(!pausedPosition.moving && pausedPosition.x == first.x && pausedPosition.y == first.y);
    const auto resumedPosition = roam.Step(100040, true);
    assert(std::hypot(resumedPosition.x - first.x, resumedPosition.y - first.y) < 1);
    for (std::uint64_t now = 100080; now < 1000000; now += 40) {
        const auto position = roam.Step(now, true);
        assert(std::hypot(position.x, position.y) <= RoamingClock::Radius);
    }
    for (auto action : {Action::Hop, Action::Dance, Action::Cheer}) {
        model.Play(action, 50000);
        assert(model.At(50420).manual && model.At(50420).pose != 28);
        assert(!model.At(51800).manual);
    }
    Settings walking; assert(!walking.roaming); walking.opening=false; walking.roaming=true;
    Model roamModel;roamModel.Configure(walking,1);roamModel.Page(true,1);
    assert(roamModel.CanRoam());
    roamModel.SetActivity(Activity::Working,2);assert(!roamModel.CanRoam());
    roamModel.SetActivity(Activity::Idle,3);roamModel.Play(Action::Wave,3);assert(!roamModel.CanRoam());
    roamModel.Stop(4);walking.roaming=false;roamModel.Configure(walking,4);assert(!roamModel.CanRoam());
    std::cout << "AI pet: delayed entry, three hops before single-hand punches, cancellation, pause and state "
                 "restoration passed\n";
}
