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
    assert(model.At(23000).pose == 33 && !model.At(23000).manual);

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

    Model changedWhilePaused;
    changedWhilePaused.Page(true, 0);
    changedWhilePaused.At(1000, false);
    changedWhilePaused.SetActivity(Activity::Done, 3000);
    assert(changedWhilePaused.At(5000, true).pose == 34);
    assert(changedWhilePaused.At(6000, true).pose == 28);
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
    assert(model.At(36501).pose == 35);
    model.SetActivity(Activity::Stopped, 37000);
    assert(model.At(37001).pose == 35);
    model.SetActivity(Activity::Done, 38000);
    assert(model.At(38100).pose == 34);
    assert(model.At(40000).pose == 28);
    model.SetActivity(Activity::Done, 40100);
    assert(model.At(40200).pose == 28);

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
    assert(model.At(44000).pose == 28);
    std::cout << "AI pet: delayed entry, three hops before single-hand punches, cancellation, pause and state "
                 "restoration passed\n";
}
