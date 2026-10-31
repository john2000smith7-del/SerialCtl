#include "QueuedConnection.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
void Require(bool value,const char *why) { if(!value)throw std::runtime_error(why); }
struct HeldWire final : serialctl::IConnection {
    HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE resized=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic_bool connected{false},holdResize{false};
    std::mutex wire;
    serialctl::Bytes sent;
    int columns=0,rows=0,resizeCalls=0;
    ~HeldWire() override { CloseHandle(entered);CloseHandle(release);CloseHandle(resized); }
    bool Start(serialctl::DataCallback,serialctl::StatusCallback,std::wstring&) override { connected=true;return true; }
    void Stop() override { connected=false;SetEvent(release); }
    bool IsConnected() const override { return connected; }
    bool Send(const serialctl::Bytes &bytes,std::wstring&) override {
        std::lock_guard<std::mutex> lock(wire);
        if(!sent.size()) { SetEvent(entered);WaitForSingleObject(release,4000); }
        if(!connected)return false;
        sent.insert(sent.end(),bytes.begin(),bytes.end());return true;
    }
    void ResizeTerminal(int c,int r) override {
        std::lock_guard<std::mutex> lock(wire);
        if(holdResize.exchange(false)) { SetEvent(entered);WaitForSingleObject(release,4000); }
        columns=c;rows=r;++resizeCalls;SetEvent(resized);
    }
};
}
int main() {
    int result=0;
    auto inner=std::make_unique<HeldWire>();auto held=inner.get();
    serialctl::QueuedConnection connection(std::move(inner));
    HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread watchdog([&] { if(WaitForSingleObject(done,2000)==WAIT_TIMEOUT)SetEvent(held->release); });
    try {
        std::wstring error;Require(connection.Start({}, {},error),"Cannot start controlled transport");
        Require(connection.Send({'A'},error),"Cannot queue first input");
        Require(WaitForSingleObject(held->entered,1000)==WAIT_OBJECT_0,"Send did not reach controlled wire");
        auto begin=std::chrono::steady_clock::now();
        for(int i=0;i<1000;++i)connection.ResizeTerminal(80+i,24+i);
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        std::cout<<"RESIZE_CALLER_1000_MS="<<elapsed<<'\n';
        Require(elapsed<100,"Resize caller waited for the blocked transport writer");
        SetEvent(done);watchdog.join();
        Require(connection.Send({'B'},error),"Cannot queue second input");
        SetEvent(held->release);
        Require(WaitForSingleObject(held->resized,1000)==WAIT_OBJECT_0,"Latest queued size never reached transport");
        const auto deadline=GetTickCount64()+1000;
        bool delivered=false;
        while(GetTickCount64()<deadline) {
            { std::lock_guard<std::mutex> lock(held->wire);
              delivered=held->sent==serialctl::Bytes({'A','B'}) && held->columns==1079 && held->rows==1023;
              if(delivered)Require(held->resizeCalls==1,"Intermediate resize notifications were not coalesced"); }
            if(delivered)break;Sleep(1);
        }
        Require(delivered,"Latest size or accepted byte ordering changed");
        ResetEvent(held->entered);ResetEvent(held->release);held->holdResize=true;
        connection.ResizeTerminal(132,43);
        Require(WaitForSingleObject(held->entered,1000)==WAIT_OBJECT_0,"Resize worker did not enter controlled wait");
        begin=std::chrono::steady_clock::now();
        for(int i=0;i<1000;++i)connection.ResizeTerminal(100+i,40+i);
        const auto blockedResize=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        Require(blockedResize<100,"Resize caller waited for an earlier resize notification");
        Require(connection.Send({'C'},error),"Cannot queue input behind held resize");
        connection.Stop();
        Require(held->sent==serialctl::Bytes({'A','B'}),"Stop replayed a pending input");
        std::cout<<"RESIZE_HELD_CALLER_1000_MS="<<blockedResize<<"\nPASS: blocked wire and blocked resize stay off the caller, latest size coalesces, input FIFO and stop hold\n";
    } catch(const std::exception &error) { std::cerr<<error.what()<<'\n';result=1; }
    SetEvent(done);SetEvent(held->release);if(watchdog.joinable())watchdog.join();
    connection.Stop();CloseHandle(done);return result;
}
