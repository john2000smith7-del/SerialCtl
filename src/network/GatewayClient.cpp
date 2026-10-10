#include "GatewayClient.h"
#include "NetworkConnect.h"
namespace serialctl {
void GatewayClient::Connect(const std::wstring& host,unsigned port,const std::atomic_bool* cancel,DWORD timeout){
    Close();std::wstring error;socket_=ConnectTcpSocket(host,static_cast<std::uint16_t>(port),error,cancel,timeout);if(socket_==INVALID_SOCKET)throw std::runtime_error(WideToMultiByte(error,CP_UTF8));
    try{auto tail=ws::UpgradeClient(socket_,host,port);stream_=std::make_unique<ws::Stream>(socket_,true,std::move(tail));hello_=Receive();if(hello_.value("type","")!="hello"||hello_.value("version",0)!=1)throw std::runtime_error("PROTOCOL_MISMATCH");}
    catch(...){Close();throw;}
}
void GatewayClient::Close(){Cancel();stream_.reset();auto s=socket_.exchange(INVALID_SOCKET);if(s!=INVALID_SOCKET)closesocket(s);}
std::string GatewayClient::SendRequest(const std::string& op,const std::string& resource,nlohmann::json params){
    std::lock_guard<std::mutex> l(send_);auto id=std::to_string(++request_);auto j=nlohmann::json{{"type","request"},{"version",1},{"instance",hello_.at("instance")},{"requestId",id},{"op",op},{"params",params}};if(!resource.empty())j["resource"]=resource;auto text=j.dump();if(!stream_->Send(1,Bytes(text.begin(),text.end())))throw std::runtime_error("SEND_FAILED; execution may be unknown");return id;
}
nlohmann::json GatewayClient::Request(const std::string& op,const std::string& resource,nlohmann::json params){auto id=SendRequest(op,resource,params);for(;;){auto j=Receive();if(j.value("type","")=="response"&&j.value("requestId","")==id){if(!j.value("ok",false))throw std::runtime_error(j.at("error").dump());return j.at("result");}throw std::runtime_error("Unexpected frame during synchronous request");}}
nlohmann::json GatewayClient::Receive(){for(;;){auto m=stream_->Receive();if(m.opcode==9){std::lock_guard<std::mutex> l(send_);if(!stream_->Send(10,m.data))throw std::runtime_error("PONG_FAILED");continue;}if(m.opcode==10)continue;if(m.opcode==8)throw std::runtime_error("CLOSED: "+std::string(m.data.begin()+(m.data.size()>=2?2:0),m.data.end()));if(m.opcode!=1)throw std::runtime_error("PROTOCOL_MISMATCH");return nlohmann::json::parse(m.data.begin(),m.data.end());}}
} // namespace serialctl
