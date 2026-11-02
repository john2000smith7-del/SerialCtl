#include "BootInterrupt.h"
#include <iostream>
#include <stdexcept>
using serialctl::BootInterrupt;
static void Check(bool value,const char* note){if(!value)throw std::runtime_error(note);}
int main() {
    try {
        BootInterrupt boot;
        Check(boot.PreparePreset("uboot_space",0,100,300),"legacy preset must remain accepted");
        Check(boot.Status(100).preset=="uboot_stop"&&boot.Status(100).intervalMs==500,"legacy cadence migrates to requested 500ms");
        Check(boot.Poll(100).empty(),"prepared state must not precede restart");
        boot.RestartSubmitted(100);
        const auto burst=boot.PollBurst(100);
        Check(burst.bytes==" stop"&&boot.CanSend(burst.generation),"exact interrupt bytes and live generation");
        Check(boot.Poll(599).empty()&&boot.Poll(600)==" stop","500ms wire cadence");
        const std::string banner="\r\nU-Boot 2024\r\nHit any key to stop autoboot: 3\b2\b1\r\n";
        boot.Feed(banner.data(),banner.size(),601);
        Check(!boot.Status(601).entered,"banner and countdown cannot count as entry");
        const std::string prompt="\r\n \x1b[32m=>\x1b[0m stop";
        for(std::size_t i=0;i<prompt.size();++i)boot.Feed(prompt.data()+i,1,602+i);
        Check(boot.Status(700).entered&&boot.Status(700).state=="entered","fragmented colored/indented prompt must stop natively");
        Check(!boot.CanSend(burst.generation)&&boot.Poll(1200).empty(),"queued old bursts become invalid immediately");
        boot.Delivery(false,"a late queued write was cancelled");
        Check(boot.Status(1200).entered&&boot.Status(1200).state=="entered","late write cancellation cannot erase observed entry");
        Check(boot.PreparePreset("phytium_uefi_f2",0,1300),"next task may start after entered");
        boot.RestartSubmitted(1300);
        Check(!boot.CanSend(burst.generation)&&boot.Poll(1300)=="\x1bOQ"&&boot.Status(1300).intervalMs==300,"old generation cannot spill into unchanged UEFI strategy");
        boot.Cancel();
        for(const auto* named:{"U-Boot>","U-Boot #","hisilicon #"}) {
            Check(boot.PreparePreset("uboot_stop",0,2000),"named prompt prepare");
            boot.RestartSubmitted(2000);boot.Poll(2000);
            const std::string prose=std::string("\r\nexpected console prompt: ")+named+" \r\n";
            boot.Feed(prose.data(),prose.size(),2001);
            Check(!boot.Status(2001).entered,"quoted prompt in prose cannot stop");
            const std::string actual=std::string("\r\n")+named+" ";
            boot.Feed(actual.data(),actual.size(),2002);
            Check(boot.Status(2002).entered&&boot.Poll(3000).empty(),"named real prompt must stop");
        }
        Check(boot.PreparePreset("uboot_stop",0,4000),"OSC prepare");boot.RestartSubmitted(4000);boot.Poll(4000);
        const std::string title="\x1b]0;=> fake title\aBooting Linux\r\n";
        boot.Feed(title.data(),title.size(),4001);
        Check(!boot.Status(4001).entered,"OSC title cannot fake a prompt");
        boot.Cancel();
        Check(boot.Poll(5000).empty(),"explicit cancel stops all later output");
        std::cout<<"PASS: exact stop bytes/500ms, ANSI chunking, prompt evidence, cancellation generations and unchanged UEFI\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

