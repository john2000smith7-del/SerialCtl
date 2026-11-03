#include "TransferProtocol.h"
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <fstream>
#include <iostream>
using namespace serialctl::xfer;
int main(int argc,char** argv) {
    if(argc<5)return 2;
    Options o;o.direction=std::string(argv[1])=="send"?Direction::Send:Direction::Receive;
    const std::string protocol=argv[2];o.protocol=protocol=="x"?Protocol::Xmodem:protocol=="y"?Protocol::Ymodem:protocol=="z"?Protocol::Zmodem:Protocol::Raw;
    o.filename="fixture.bin";o.timeoutMs=1500;o.retries=10;o.expectedReceiveBytes=std::stoull(argv[4]);o.receiveLimit=8*1024*1024;
    std::ifstream source;std::ofstream dest;
    if(o.direction==Direction::Send) {source.open(argv[3],std::ios::binary);source.seekg(0,std::ios::end);o.size=source.tellg();}
    else dest.open(argv[3],std::ios::binary);
    Io io;
    io.readByte=[](unsigned ms){pollfd p{0,POLLIN,0};const int n=poll(&p,1,static_cast<int>(ms));if(n<0&&errno!=EINTR)throw std::runtime_error("poll failure");if(n<=0)return -1;unsigned char b;const auto got=read(0,&b,1);if(got==1)return static_cast<int>(b);throw std::runtime_error("wire closed");};
    io.write=[](const Bytes& bytes){std::size_t offset=0;while(offset<bytes.size()){const auto n=write(1,bytes.data()+offset,bytes.size()-offset);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("wire write failed");offset+=static_cast<std::size_t>(n);}};
    io.readFile=[&](std::uint64_t offset,std::uint8_t* out,std::size_t n){source.clear();source.seekg(static_cast<std::streamoff>(offset));source.read(reinterpret_cast<char*>(out),static_cast<std::streamsize>(n));return static_cast<std::size_t>(source.gcount());};
    io.writeFile=[&](const std::uint8_t* in,std::size_t n){dest.write(reinterpret_cast<const char*>(in),static_cast<std::streamsize>(n));if(!dest)throw std::runtime_error("file write failed");};
    io.abortWire=[](){const unsigned char bytes[]={24,24,24,24,24,24,24,24};(void)write(1,bytes,sizeof(bytes));};
    try {const auto result=Run(o,io);std::cerr<<"PASS bytes="<<result.bytes<<" peer="<<result.peerAcknowledged<<"\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
