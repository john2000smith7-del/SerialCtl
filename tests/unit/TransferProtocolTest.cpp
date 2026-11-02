#include "TransferProtocol.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
using namespace serialctl::xfer;
namespace {
void Require(bool value,const char* note) {if(!value)throw std::runtime_error(note);}
struct Pipe {
    std::mutex mutex;std::condition_variable wake;std::deque<int> bytes;std::atomic_bool* cancel{};
    int Read(unsigned ms) {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock,std::chrono::milliseconds(ms),[&]{return !bytes.empty()||*cancel;});
        if(bytes.empty())return -1;
        int c=bytes.front();bytes.pop_front();return c;
    }
    void Write(const Bytes& b) {std::lock_guard<std::mutex> lock(mutex);bytes.insert(bytes.end(),b.begin(),b.end());wake.notify_all();}
};
void Pair(Protocol protocol,std::size_t size,bool corruption=false,bool dropAck=false) {
    std::cout<<"pair protocol="<<static_cast<int>(protocol)<<" size="<<size<<" corrupt="<<corruption<<" dropAck="<<dropAck<<std::endl;
    std::atomic_bool cancelled{false};
    Pipe a,b;a.cancel=b.cancel=&cancelled;
    Bytes original(size),received;
    for(std::size_t i=0;i<size;++i)original[i]=static_cast<std::uint8_t>((i*173+i/7)&255);
    bool corrupted=false,dropped=false;
    Io send,receive;
    send.cancelled=receive.cancelled=[&]{return cancelled.load();};
    send.readByte=[&](unsigned ms){return a.Read(ms);};
    receive.readByte=[&](unsigned ms){return b.Read(ms);};
    send.write=[&](const Bytes& bytes) {
        Bytes wire=bytes;
        if(corruption&&!corrupted&&bytes.size()>300) {wire[150]^=1;corrupted=true;}
        b.Write(wire);
    };
    receive.write=[&](const Bytes& bytes) {
        if(dropAck&&!dropped&&bytes==Bytes{6}) {dropped=true;return;}
        a.Write(bytes);
    };
    send.readFile=[&](std::uint64_t offset,std::uint8_t* out,std::size_t count){std::copy_n(original.data()+offset,count,out);return count;};
    receive.writeFile=[&](const std::uint8_t* in,std::size_t count){received.insert(received.end(),in,in+count);};
    receive.metadata=[&](const std::string& name,std::uint64_t n,bool known) {
        if(protocol==Protocol::Ymodem||protocol==Protocol::Zmodem)Require(name=="fixture.bin"&&known&&n==size,"peer metadata");
    };
    Options tx;tx.protocol=protocol;tx.direction=Direction::Send;tx.filename="fixture.bin";tx.size=size;tx.timeoutMs=250;tx.retries=10;
    Options rx=tx;rx.direction=Direction::Receive;rx.expectedReceiveBytes=size;rx.receiveLimit=size+1024;
    auto future=std::async(std::launch::async,[&]{try{return Run(rx,receive);}catch(...){cancelled=true;a.wake.notify_all();b.wake.notify_all();throw;}});
    Result sent;
    try {sent=Run(tx,send);}catch(...) {cancelled=true;a.wake.notify_all();b.wake.notify_all();future.wait();throw;}
    if(future.wait_for(std::chrono::seconds(10))!=std::future_status::ready) {cancelled=true;a.wake.notify_all();b.wake.notify_all();future.wait();throw std::runtime_error("receiver did not finish");}
    const auto got=future.get();
    Require(received==original,"binary payload changed, lost, duplicated or padded");
    Require(sent.bytes==size&&got.bytes==size,"byte count");
    if(protocol!=Protocol::Raw)Require(sent.peerAcknowledged&&got.peerAcknowledged,"protocol finish acknowledgement");
    else Require(!sent.peerAcknowledged&&!got.peerAcknowledged,"raw must not claim peer verification");
    if(corruption)Require(corrupted,"corruption must actually run");
    if(dropAck)Require(dropped,"lost ACK must actually run");
}
void RejectMetadata(const std::string& text) {
    Bytes block(128,0);std::copy(text.begin(),text.end(),block.begin());
    Bytes wire{1,0,255};wire.insert(wire.end(),block.begin(),block.end());
    const auto crc=Crc16(block.data(),block.size());wire.push_back(static_cast<std::uint8_t>(crc>>8));wire.push_back(static_cast<std::uint8_t>(crc));
    std::size_t offset=0;bool wrote=false;
    Io io;io.readByte=[&](unsigned){return offset<wire.size()?wire[offset++]:-1;};io.write=[](const Bytes&){};
    io.writeFile=[&](const std::uint8_t*,std::size_t){wrote=true;};
    Options o;o.protocol=Protocol::Ymodem;o.direction=Direction::Receive;o.timeoutMs=50;o.retries=1;
    bool rejected=false;try {Run(o,io);}catch(const std::exception&) {rejected=true;}
    Require(rejected&&!wrote,"bad metadata must fail before file bytes are written");
}
void MalformedStreams() {
    // Deterministic, bounded hostile byte streams. This checks parser memory
    // safety under sanitizers; a synthetic ACK is not an independent peer.
    std::uint32_t seed=0x219500;
    for(unsigned sample=0;sample<1200;++sample) {
        const auto random=[&](){seed=seed*1664525u+1013904223u;return seed;};
        Bytes input(1+random()%2048);for(auto& b:input)b=static_cast<std::uint8_t>(random()>>16);
        if(sample%3==0)input.insert(input.begin(),{'*','*',24,'C'}); // binary CRC32 header path
        if(sample%3==1)input.insert(input.begin(),{1,0,255});       // metadata/block path
        std::size_t cursor=0,emitted=0,saved=0;
        Io io;io.readByte=[&](unsigned)->int {if(cursor==input.size())throw std::runtime_error("controlled input ended");return input[cursor++];};
        io.write=[&](const Bytes& b){emitted+=b.size();Require(emitted<=1024*1024,"unbounded malformed-stream response");};
        io.readFile=[](std::uint64_t at,std::uint8_t* out,std::size_t n){for(std::size_t i=0;i<n;++i)out[i]=static_cast<std::uint8_t>(at+i);return n;};
        io.writeFile=[&](const std::uint8_t*,std::size_t n){saved+=n;Require(saved<=4096,"malformed stream exceeded receive limit");};
        Options options;options.protocol=static_cast<Protocol>(1+sample%3);
        options.direction=sample%2?Direction::Send:Direction::Receive;
        options.filename="fixture.bin";options.size=32;options.receiveLimit=4096;options.expectedReceiveBytes=32;options.timeoutMs=50;options.retries=1;
        try {Run(options,io);}catch(const std::runtime_error&) {}
        Require(cursor<=input.size()&&emitted<=1024*1024&&saved<=4096,"malformed-stream bounds");
    }
    std::cout<<"PASS: 1200 deterministic malformed-stream parser cases (seed 0x219500); no hardware/peer authenticity claim\n";
}
}
int main() {
    try {
        const std::string standard="123456789";
        Require(Crc16(reinterpret_cast<const std::uint8_t*>(standard.data()),standard.size())==0x31c3,"independent CRC16/XMODEM check vector");
        Require(Crc32(reinterpret_cast<const std::uint8_t*>(standard.data()),standard.size())==0xcbf43926,"independent IEEE CRC32 check vector");
        for(auto protocol:{Protocol::Xmodem,Protocol::Ymodem,Protocol::Zmodem})
            for(auto size:{0u,1u,127u,128u,129u,1024u,65539u})Pair(protocol,size);
        Pair(Protocol::Raw,8193);
        Pair(Protocol::Xmodem,1025,false,true);
        Pair(Protocol::Ymodem,8193,true);
        Pair(Protocol::Ymodem,8193,false,true);
        Pair(Protocol::Zmodem,8193,true);
        RejectMetadata(std::string("../escape.bin")+char(0)+"10");
        RejectMetadata(std::string("fixture.bin")+char(0)+"18446744073709551616");
        RejectMetadata(std::string("fixture.bin")+char(0)+"999999999999");
        MalformedStreams();
        Io cancelled;cancelled.cancelled=[]{return true;};Options options;
        bool stopped=false;try {Run(options,cancelled);}catch(const Cancelled&){stopped=true;}
        Require(stopped,"cancellation before any wire/file operation");
        std::cout<<"PASS: CRC vectors, binary boundaries, sequence wrap, corruption/lost ACK, receive limits and cancellation\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}

