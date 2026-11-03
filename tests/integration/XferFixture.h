#pragma once
#include "ApiServer.h"
#include "SessionService.h"
#include "QueuedConnection.h"
#include "TransferController.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace serialctl::testing {
class XferWire final:public IConnection {
    xfer::Protocol protocol_;
    xfer::Direction direction_;
    Bytes payload_;
    std::atomic_bool connected_{false},cancel_{false},done_{true};
    DataCallback incoming_;
    std::thread peer_;
    std::mutex mutex_,reportMutex_;
    std::condition_variable wake_;
    std::deque<std::uint8_t> rx_;
    Bytes received_;
    std::atomic_uint64_t written_{0};
    unsigned delayMs_=0;
    Json report_={{"status","IDLE"}};
  public:
    XferWire(xfer::Protocol p,xfer::Direction d,Bytes bytes,unsigned delayMs=0):protocol_(p),direction_(d),payload_(std::move(bytes)),delayMs_(std::min(delayMs,50u)){}
    ~XferWire(){Stop();}
    bool SupportsTransfer()const override{return true;}
    bool Start(DataCallback data,StatusCallback,std::wstring&)override{incoming_=std::move(data);connected_=true;return true;}
    bool IsConnected()const override{return connected_;}
    void Stop()override{connected_=false;cancel_=true;wake_.notify_all();if(peer_.joinable())peer_.join();}
    bool Send(const Bytes& bytes,std::wstring&)override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!connected_||bytes.size()>2*1024*1024-rx_.size())return false;
        rx_.insert(rx_.end(),bytes.begin(),bytes.end());written_+=bytes.size();wake_.notify_all();return true;
    }
    bool BeginTransferWire(const std::string&,std::wstring& error)override {
        if(!done_){error=L"Controlled peer already active";return false;}
        if(peer_.joinable())peer_.join();
        {std::lock_guard<std::mutex> lock(mutex_);rx_.clear();}
        received_.clear();cancel_=false;done_=false;
        peer_=std::thread([this]{
            xfer::Options o;o.protocol=protocol_;o.direction=direction_;o.filename="fixture.bin";
            o.size=payload_.size();o.expectedReceiveBytes=payload_.size();o.timeoutMs=2500;o.retries=4;o.receiveLimit=8*1024*1024;
            xfer::Io io;
            io.cancelled=[this]{return cancel_.load();};
            io.readByte=[this](unsigned ms) {
                std::unique_lock<std::mutex> lock(mutex_);wake_.wait_for(lock,std::chrono::milliseconds(ms),[this]{return !rx_.empty()||cancel_;});
                if(rx_.empty())return -1;
                const int c=rx_.front();rx_.pop_front();return c;
            };
            io.write=[this](const Bytes& bytes){if(incoming_)incoming_(bytes);if(delayMs_)Sleep(delayMs_);};
            io.readFile=[this](std::uint64_t at,std::uint8_t* out,std::size_t n){std::copy_n(payload_.data()+at,n,out);return n;};
            io.writeFile=[this](const std::uint8_t* bytes,std::size_t n){received_.insert(received_.end(),bytes,bytes+n);if(delayMs_)Sleep(delayMs_);};
            Json report;
            try {
                const auto result=xfer::Run(o,io);
                const bool exact=direction_==xfer::Direction::Send||received_==payload_;
                report={{"status",exact?"PASS":"FAIL"},{"bytes",result.bytes},{"exactBytes",exact},{"peerAcknowledged",result.peerAcknowledged}};
            }catch(const std::exception& e){report={{"status","FAIL"},{"error",e.what()},{"bytes",received_.size()}};}
            {std::lock_guard<std::mutex> lock(reportMutex_);report_=std::move(report);}
            done_=true;wake_.notify_all();
        });
        return true;
    }
    bool EndTransferWire(const std::string&,std::wstring&)override {
        {std::unique_lock<std::mutex> lock(mutex_);wake_.wait_for(lock,std::chrono::seconds(1),[this]{return done_.load();});}
        if(!done_){cancel_=true;wake_.notify_all();}
        if(peer_.joinable())peer_.join();return true;
    }
    void CancelTransferWire(const std::string&)override{cancel_=true;wake_.notify_all();}
    Json Report(){std::lock_guard<std::mutex> lock(reportMutex_);return report_;}
    std::uint64_t Written()const{return written_;}
};
class XferGateway {
    WSADATA wsa_{};
    SessionService service_;
    ApiServer api_;
    std::shared_ptr<QueuedConnection> queue_;
    XferWire* wire_=nullptr;
  public:
    XferGateway(){if(WSAStartup(MAKEWORD(2,2),&wsa_))throw std::runtime_error("WSAStartup");}
    ~XferGateway(){if(queue_)queue_->Stop();api_.Stop();WSACleanup();}
    bool Start(xfer::Protocol protocol,xfer::Direction direction,const Bytes& bytes,unsigned first,unsigned last,std::wstring& error,unsigned delayMs=0) {
        auto owned=std::make_unique<XferWire>(protocol,direction,bytes,delayMs);wire_=owned.get();queue_=std::make_shared<QueuedConnection>(std::move(owned));
        service_.ObserveTransfer([this](const std::string& id,const std::string& owner,const Bytes& data){api_.EnqueuePublish(id,data,owner.c_str(),"xfer_output");});
        service_.Register({{"id","serial-xfer-fixture"},{"kind","serial"},{"com","COM77"},{"name","Controlled Xfer COM77"},{"baudRate",115200},{"codePage",65001}},queue_);
        if(!queue_->Start([this](const Bytes& data){api_.EnqueuePublish("serial-xfer-fixture",data);},{},error))return false;
        if(!api_.Start([this](const std::string& op,const std::string& id,const Json& p)->Json {
            if(op=="resources")return {{"resources",service_.Resources()}};
            if(op=="session.get")return service_.State(id);
            if(op=="input")return service_.Input(id,ws::Unbase64(p.at("data")),p.value("source",std::string()));
            if(op=="transfer.release_owner"){service_.ReleaseTransfers(p.value("source",std::string()));return {{"released",true}};}
            if(op=="transfer.begin"||op=="transfer.write"||op=="transfer.end")
                return service_.Transfer(op,id,p.at("source").get<std::string>()+"/"+p.at("token").get<std::string>(),op=="transfer.write"?ws::Unbase64(p.at("data")):Bytes{});
            return {{"error",{{"code","UNSUPPORTED"},{"message","Controlled test fixture"}}}};
        },error,first,last))return false;
        api_.SetSessions({"serial-xfer-fixture"});return true;
    }
    unsigned Port()const{return api_.Port();}
    Json Report(){return wire_->Report();}
    std::uint64_t Written()const{return wire_->Written();}
    bool Busy()const{return queue_&&queue_->TransferActive();}
    Json History(){return api_.Events("serial-xfer-fixture",0);}
};
} // namespace serialctl::testing
