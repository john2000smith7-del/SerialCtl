#include "QueuedConnection.h"
#include "BootInterrupt.h"
#include <iostream>
#include <stdexcept>
using namespace serialctl;
namespace {
void Check(bool value,const char* note){if(!value)throw std::runtime_error(note);}
struct Wire final:IConnection {
    std::atomic_bool connected{false};
    DataCallback incoming;
    HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr),release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::mutex mutex;
    Bytes sent;
    unsigned prepared=0;
    ~Wire(){CloseHandle(entered);CloseHandle(release);}
    bool Start(DataCallback data,StatusCallback,std::wstring&)override{incoming=std::move(data);connected=true;return true;}
    void Stop()override{connected=false;SetEvent(release);}
    bool IsConnected()const override{return connected;}
    Bytes PrepareInput(const Bytes& b)override{++prepared;return b;}
    bool BeginTransferWire(const std::string&,std::wstring&)override{return connected;}
    bool Send(const Bytes& b,std::wstring&)override {
        SetEvent(entered);WaitForSingleObject(release,2000);
        if(!connected)return false;
        std::lock_guard<std::mutex> lock(mutex);sent.insert(sent.end(),b.begin(),b.end());return true;
    }
};
}
int main() {
    try {
        auto owned=std::make_unique<Wire>();auto* wire=owned.get();
        QueuedConnection queue(std::move(owned));
        Bytes normal,captured;std::wstring error;
        Check(queue.Start([&](const Bytes& b){normal.insert(normal.end(),b.begin(),b.end());},{},error),"start");
        Check(queue.Send({'A'},error),"held ordinary input");
        Check(WaitForSingleObject(wire->entered,1000)==WAIT_OBJECT_0,"ordinary writer actually in flight");
        Check(!queue.BeginTransfer("owner",{},error),"in-flight writes cannot be stolen by transfer");
        auto boot=std::make_shared<BootInterrupt>();boot->PreparePreset("uboot_stop",0,100);boot->RestartSubmitted(100);
        const auto burst=boot->PollBurst(100);
        Check(queue.SendConditional(Bytes(burst.bytes.begin(),burst.bytes.end()),error,[boot,g=burst.generation]{return boot->CanSend(g);}),"queue boot interrupt behind actual blocked writer");
        boot->Feed("\r\n=> ",5,101);
        SetEvent(wire->release);
        bool leased=false;
        for(int i=0;i<1000&&!leased;++i) {
            leased=queue.BeginTransfer("owner",[&](const Bytes& b){captured.insert(captured.end(),b.begin(),b.end());},error,[&]{Bytes remainder=captured;captured.clear();return remainder;});
            if(!leased)Sleep(1);
        }
        Check(leased,"transfer lease after ordinary queue drained");
        {std::lock_guard<std::mutex> lock(wire->mutex);Check(wire->sent==Bytes{'A'},"observed prompt must invalidate queued stop sequence before wire write");}
        Check(!queue.Send({'B'},error),"other input must not interleave");
        Check(!queue.SendTransfer("other",Bytes{0,255},error),"another owner must not write");
        const auto prepared=wire->prepared;
        Check(queue.SendTransfer("owner",Bytes{0,255,26},error),"exclusive binary write");
        Check(wire->prepared==prepared,"transfer must bypass text/input preparation");
        wire->incoming(Bytes{'x','y'});
        Check(captured==Bytes({'x','y'})&&normal.empty(),"protocol receive bypasses terminal decoder");
        Check(!queue.EndTransfer("other",error),"another owner must not release");
        Check(queue.EndTransfer("owner",error)&&normal==Bytes({'x','y'}),"unconsumed tail returns once on release");
        Check(queue.BeginTransfer("second",[&](const Bytes& b){captured=b;},error),"new generation can acquire");
        Check(!queue.SendTransfer("owner",{'q'},error),"old owner cannot write into later transfer");
        Check(queue.EndTransfer("second",error),"new owner release");
        wire->incoming(Bytes{'z'});
        Check(normal==Bytes({'x','y','z'}),"ordinary reception restored");
        queue.Stop();
        Check(!queue.BeginTransfer("closed",{},error),"stopped transport cannot acquire");
        std::cout<<"PASS: serial transfer ownership, exact raw bytes, receive restoration and no queued boot keys after prompt\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

