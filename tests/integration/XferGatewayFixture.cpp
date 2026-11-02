#include "XferFixture.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace serialctl;
int wmain(int argc,wchar_t** argv) {
    if(argc!=6&&argc!=7)return 2;
    try {
        const auto p=std::stoi(argv[2]);const auto d=std::wstring(argv[3])==L"receive"?xfer::Direction::Receive:xfer::Direction::Send;
        std::ifstream in(std::filesystem::path(argv[4]),std::ios::binary);Bytes bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
        testing::XferGateway gateway;std::wstring error;
        if(!gateway.Start(static_cast<xfer::Protocol>(p),d,bytes,std::stoul(argv[1]),std::stoul(argv[1]),error,
            argc==7?static_cast<unsigned>(std::stoul(argv[6])):0)){std::wcerr<<error;return 1;}
        std::cout<<"READY "<<gateway.Port()<<std::endl;
        const auto deadline=GetTickCount64()+180000;
        std::string last;
        while(GetTickCount64()<deadline) {
            auto report=gateway.Report();const auto text=report.dump();
            if(text!=last){std::ofstream out(std::filesystem::path(argv[5]),std::ios::binary|std::ios::trunc);out<<text;last=text;}
            Sleep(20);
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
