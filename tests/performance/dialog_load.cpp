#include <windows.h>
#include <cstdio>
int main() {
    // Fill real CMD terminal history, then maintain a bounded stream for the test.
    for(int i=0;i<22000;++i)std::printf("history_%06d 0123456789abcdefghijklmnopqrstuvwxyz\r\n",i);
    std::fflush(stdout);
    for(int batch=0;batch<1800;++batch){
        for(int line=0;line<20;++line)std::printf("SERIALCTL_PAINT_%03d batch=%d line=%d 0123456789abcdefghijklmnopqrstuvwxyz\r\n",batch%1000,batch,line);
        std::fflush(stdout);Sleep(50);
    }
    return 0;
}
