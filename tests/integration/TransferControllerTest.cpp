#include "XferFixture.h"
#include "SharedSerialConnection.h"
#include "FilePaths.h"
#include <fstream>
#include <filesystem>
#include <iostream>
using namespace serialctl;
static void Check(bool ok,const char* note){if(!ok)throw std::runtime_error(note);}
static void Write(const std::wstring& path,const Bytes& bytes) {
    std::ofstream out(std::filesystem::path(path),std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if(!out)throw std::runtime_error("fixture source write");
}
namespace {
struct ManualWire final:IConnection {
    std::atomic_bool connected{false};
    bool blockWrite=false;
    DataCallback incoming;
    HANDLE begun=CreateEventW(nullptr,TRUE,FALSE,nullptr),writing=CreateEventW(nullptr,TRUE,FALSE,nullptr),
           cancelled=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic_uint leases{0},releases{0};
    ~ManualWire(){Stop();CloseHandle(begun);CloseHandle(writing);CloseHandle(cancelled);}
    bool SupportsTransfer()const override{return true;}
    bool Start(DataCallback callback,StatusCallback,std::wstring&)override{incoming=std::move(callback);connected=true;return true;}
    void Stop()override{connected=false;SetEvent(cancelled);}
    bool IsConnected()const override{return connected;}
    bool Send(const Bytes&,std::wstring&)override{
        SetEvent(writing);
        if(blockWrite)WaitForSingleObject(cancelled,5000);
        return connected&&WaitForSingleObject(cancelled,0)!=WAIT_OBJECT_0;
    }
    bool BeginTransferWire(const std::string&,std::wstring&)override{++leases;ResetEvent(cancelled);SetEvent(begun);return true;}
    bool EndTransferWire(const std::string&,std::wstring&)override{++releases;return true;}
    void CancelTransferWire(const std::string&)override{SetEvent(cancelled);}
};
void WaitDone(xfer::TransferController& controller) {
    const auto deadline=GetTickCount64()+2000;
    while(controller.Busy()&&GetTickCount64()<deadline)Sleep(1);
    Check(!controller.Busy(),"controller cleanup exceeded 2 seconds");
    controller.Shutdown();
}
void EdgeCases(const std::wstring& folder) {
    for(int scenario=0;scenario<4;++scenario) {
        auto owned=std::make_unique<ManualWire>();auto* wire=owned.get();
        wire->blockWrite=scenario==1;
        auto queue=std::make_shared<QueuedConnection>(std::move(owned));
        Bytes ordinary;std::mutex ordinaryMutex;std::wstring error;
        Check(queue->Start([&](const Bytes& b){std::lock_guard<std::mutex> lock(ordinaryMutex);ordinary.insert(ordinary.end(),b.begin(),b.end());},{},error),"edge queue start");
        const auto path=folder+L"\\边界-"+std::to_wstring(scenario)+L".bin";
        xfer::TransferController controller;
        xfer::TransferController::Request request;
        request.session=10;request.source=L"controlled manual wire";request.path=path;request.connection=queue;
        request.options.protocol=xfer::Protocol::Raw;
        request.options.direction=scenario==1?xfer::Direction::Send:xfer::Direction::Receive;
        const Bytes original(1025,0xe4);
        if(scenario==1||scenario==3)Write(path,original);
        Check(controller.Start(std::move(request),error),"edge controller start");
        if(scenario==3) {
            WaitDone(controller);
            Check(!controller.State().success&&wire->leases==0,"existing destination must fail before acquiring serial");
            std::ifstream in(std::filesystem::path(path),std::ios::binary);
            Check(Bytes((std::istreambuf_iterator<char>(in)),{})==original,"existing receive file changed");
        } else {
            Check(WaitForSingleObject(wire->begun,1500)==WAIT_OBJECT_0,"edge transfer really acquired");
            if(scenario==0) {
                wire->incoming(original);
                controller.FinishRawReceive();
                wire->incoming(Bytes{'T','A','I','L'});
                WaitDone(controller);
                const auto state=controller.State();
                Check(state.success&&state.bytes==original.size()&&state.rawReceiveCutoff==original.size(),"manual finish must commit exactly bytes accepted at click");
                std::ifstream in(std::filesystem::path(path),std::ios::binary);
                Check(Bytes((std::istreambuf_iterator<char>(in)),{})==original,"manual finish lost or appended bytes");
                std::lock_guard<std::mutex> lock(ordinaryMutex);
                Check(ordinary==Bytes({'T','A','I','L'}),"post-cutoff terminal bytes must return exactly once");
            } else if(scenario==1) {
                Check(WaitForSingleObject(wire->writing,1500)==WAIT_OBJECT_0,"cancel case must hold actual write");
                const auto start=GetTickCount64();controller.Cancel();
                Check(GetTickCount64()-start<100,"cancel acceptance blocked behind writer");
                WaitDone(controller);
                Check(controller.State().cancelled&&!controller.State().success,"cancelled write reported success");
            } else {
                wire->Stop();
                WaitDone(controller);
                Check(!controller.State().success,"disconnected receive reported success");
                Check(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES,"failed receive committed destination");
                Check(controller.State().partial.empty(),"empty partial file must be removed");
            }
            Check(!queue->TransferActive()&&wire->releases==1,"edge cleanup must release transfer ownership exactly once");
        }
        queue->Stop();DeleteFileW(path.c_str());
        std::cout<<"PASS native edge="<<scenario<<" (manual finish / blocked-write cancel / disconnect / no overwrite)"<<std::endl;
    }
}
void DisconnectedRemoteLease() {
    testing::XferGateway gateway;std::wstring error;
    Check(gateway.Start(xfer::Protocol::Raw,xfer::Direction::Receive,Bytes(4096,'x'),25900,25950,error),"disconnect gateway");
    auto connection=std::make_shared<QueuedConnection>(std::make_unique<SharedSerialConnection>(
        L"127.0.0.1",static_cast<std::uint16_t>(gateway.Port()),L"COM77"));
    Check(connection->Start({}, {},error),"disconnect remote connection");
    Check(connection->BeginTransfer("disconnect-owner",[](const Bytes&){},error)&&gateway.Busy(),"remote actually owns wire before disconnect");
    connection->Stop();
    const auto deadline=GetTickCount64()+3000;
    while(gateway.Busy()&&GetTickCount64()<deadline)Sleep(1);
    Check(!gateway.Busy(),"remote disconnect left a serial lease behind");
    auto next=std::make_shared<QueuedConnection>(std::make_unique<SharedSerialConnection>(
        L"127.0.0.1",static_cast<std::uint16_t>(gateway.Port()),L"COM77"));
    Check(next->Start({}, {},error)&&next->BeginTransfer("new-owner",[](const Bytes&){},error),"later remote owner cannot acquire released wire");
    Check(next->EndTransfer("new-owner",error)&&!gateway.Busy(),"later owner release");
    next->Stop();
    std::cout<<"PASS remote disconnect releases ownership and a later client can acquire"<<std::endl;
}
}
int wmain() {
    const auto folder=std::wstring(L".\\xfer-native-")+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    CreateDirectoryW(folder.c_str(),nullptr);
    try {
        Bytes payload(65539);for(std::size_t i=0;i<payload.size();++i)payload[i]=static_cast<std::uint8_t>((i*173+i/7)&255);
        for(int p=0;p<4;++p)for(int d=0;d<2;++d) {
            testing::XferGateway gateway;
            std::wstring error;
            const auto protocol=static_cast<xfer::Protocol>(p);
            Check(gateway.Start(protocol,d==0?xfer::Direction::Receive:xfer::Direction::Send,payload,25900,25950,error),"start actual WebSocket gateway and controlled wire");
            GatewayClient spectator;spectator.Connect(L"127.0.0.1",gateway.Port());
            spectator.Request("subscribe","serial-xfer-fixture",{{"after",0}});
            auto remote=std::make_unique<SharedSerialConnection>(L"127.0.0.1",static_cast<std::uint16_t>(gateway.Port()),L"COM77");
            auto queue=std::make_shared<QueuedConnection>(std::move(remote));
            Check(queue->Start({}, {},error)&&queue->SupportsTransfer(),"new remote serial capability negotiation");
            xfer::TransferController controller;
            xfer::TransferController::Request request;
            request.session=1;request.source=L"controlled remote COM77";request.path=folder+L"\\case-"+std::to_wstring(p)+L"-"+std::to_wstring(d)+L".bin";
            request.options.protocol=protocol;request.options.direction=d==0?xfer::Direction::Send:xfer::Direction::Receive;
            request.options.expectedReceiveBytes=payload.size();request.options.timeoutMs=1500;request.options.retries=4;request.connection=queue;
            if(d==0)Write(request.path,payload);
            const auto path=request.path;
            Check(controller.Start(std::move(request),error),"start background controller");
            const auto deadline=GetTickCount64()+15000;
            while(controller.Busy()&&GetTickCount64()<deadline)Sleep(10);
            if(controller.Busy()){controller.Cancel();controller.Shutdown();throw std::runtime_error("controller deadline");}
            const auto state=controller.State();
            if(!state.success)std::wcerr<<state.note<<L'\n';
            Check(state.success&&state.bytes==payload.size(),"remote transfer completion and count");
            Check(state.peerAcknowledged==(p!=0),"raw must not claim protocol acknowledgement");
            if(d==1) {
                std::ifstream in(std::filesystem::path(path),std::ios::binary);Bytes actual((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
                Check(actual==payload,"received file bytes differ");
            }
            controller.Shutdown();
            const auto report=gateway.Report();
            Check(report.value("status","")=="PASS"&&report.value("exactBytes",false),"controlled wire must independently finish same payload");
            Check(gateway.History().value("events",Json::array()).empty(),"protocol file bytes must not enter public terminal history");
            Check(spectator.Request("resources").contains("resources"),"private protocol bytes leaked to subscribed spectator");
            spectator.Close();
            const auto written=gateway.Written();
            Check(queue->Send({'o','k'},error),"ordinary remote input restored after transfer");
            const auto writeDeadline=GetTickCount64()+1000;
            while(gateway.Written()<written+2&&GetTickCount64()<writeDeadline)Sleep(1);
            Check(gateway.Written()==written+2,"ordinary input must actually reach the controlled wire after transfer");
            queue->Stop();DeleteFileW(path.c_str());
            std::cout<<"PASS remote protocol="<<p<<" direction="<<d<<" bytes="<<payload.size()<<std::endl;
        }
        EdgeCases(folder);
        DisconnectedRemoteLease();
        RemoveDirectoryW(folder.c_str());
        std::cout<<"PASS: actual native controller, private WebSocket transfer, saved binary files and ordinary input restoration; no physical serial device\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
