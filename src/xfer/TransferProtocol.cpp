#include "TransferProtocol.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace serialctl::xfer {
std::uint16_t Crc16(const std::uint8_t* p,std::size_t n) {
    std::uint16_t crc=0;
    while(n--) {crc^=static_cast<std::uint16_t>(*p++)<<8;for(int i=0;i<8;++i)crc=static_cast<std::uint16_t>((crc<<1)^((crc&0x8000)?0x1021:0));}
    return crc;
}
std::uint32_t Crc32(const std::uint8_t* p,std::size_t n) {
    std::uint32_t crc=0xffffffffu;
    while(n--) {crc^=*p++;for(int i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
    return ~crc;
}
namespace {
using Clock=std::chrono::steady_clock;
constexpr int SOH=1,STX=2,EOT=4,ACK=6,NAK=21,CAN=24,CRC='C';
constexpr int ZPAD='*',ZDLE=24,ZBIN='A',ZHEX='B',ZBIN32='C';
constexpr int ZRQINIT=0,ZRINIT=1,ZSINIT=2,ZACK=3,ZFILE=4,ZSKIP=5,ZNAK=6,ZABORT=7,ZFIN=8,ZRPOS=9,ZDATA=10,ZEOF=11,ZFERR=12;
constexpr int ZCRCE='h',ZCRCG='i',ZCRCQ='j',ZCRCW='k';
struct Timeout : std::runtime_error {Timeout():std::runtime_error("Protocol timeout") {}};
struct BadFrame : std::runtime_error {BadFrame():std::runtime_error("Invalid protocol frame or checksum") {}};
struct Frame {int kind=0,sequence=0;Bytes bytes;};
struct ZHeader {int type=0;std::uint32_t position=0;bool crc32=false;};
struct ZPacket {Bytes bytes;int end=0;};
class Engine {
    const Options& o;const Io& io;std::vector<int> unread;
    Result result;
    std::uint64_t declared=0;bool hasDeclared=false;
    void Check() const {if(io.cancelled&&io.cancelled())throw Cancelled();}
    Clock::time_point Deadline() const {return Clock::now()+std::chrono::milliseconds(o.timeoutMs);}
    int Get(Clock::time_point deadline) {
        for(;;) {
            Check();
            if(!unread.empty()) {const int c=unread.back();unread.pop_back();return c;}
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
            if(remaining<=0)throw Timeout();
            const int value=io.readByte(static_cast<unsigned>(std::min<std::int64_t>(remaining,100)));
            Check();
            if(value<0)continue;
            if(value>255)throw std::runtime_error("Invalid channel byte");
            return value;
        }
    }
    void Put(const Bytes& bytes) {Check();if(!bytes.empty())io.write(bytes);}
    void Put(int byte) {Put(Bytes{static_cast<std::uint8_t>(byte)});}
    int Control(Clock::time_point deadline) {
        int cancels=0;
        for(unsigned scanned=0;scanned<8192;++scanned) {
            const int c=Get(deadline);
            if(c==CAN) {if(++cancels==2)throw std::runtime_error("Peer cancelled transfer");continue;}
            cancels=0;if(c==ACK||c==NAK||c==CRC||c=='G'||c==EOT)return c;
        }
        throw BadFrame();
    }
    bool Handshake() {
        for(unsigned i=0;i<o.retries;++i)try {
            const int c=Control(Deadline());
            if(c=='G')throw std::runtime_error("YMODEM-G streaming is not supported; select ordinary YMODEM");
            if(c==CRC)return true;
            if(c==NAK&&o.protocol==Protocol::Xmodem)return false;
        } catch(const Timeout&) {}
        throw std::runtime_error("Receiver did not enter the selected X/YMODEM mode");
    }
    Bytes Block(int sequence,const Bytes& bytes,bool crc) {
        Bytes out{static_cast<std::uint8_t>(bytes.size()==1024?STX:SOH),static_cast<std::uint8_t>(sequence),static_cast<std::uint8_t>(255-(sequence&255))};
        out.insert(out.end(),bytes.begin(),bytes.end());
        if(crc) {const auto c=Crc16(bytes.data(),bytes.size());out.push_back(static_cast<std::uint8_t>(c>>8));out.push_back(c&255);}
        else {unsigned sum=0;for(auto b:bytes)sum+=b;out.push_back(sum&255);}
        return out;
    }
    void SendBlock(int sequence,const Bytes& bytes,bool crc) {
        for(unsigned retry=0;retry<o.retries;++retry) {
            Put(Block(sequence,bytes,crc));
            try {if(Control(Deadline())==ACK)return;}catch(const Timeout&) {}
        }
        throw std::runtime_error("Block was not acknowledged within the retry limit");
    }
    Frame ReadBlock(bool crc) {
        auto deadline=Deadline();int c=0,cancels=0;
        for(unsigned scanned=0;scanned<8192;++scanned) {
            c=Get(deadline);
            if(c==CAN) {if(++cancels==2)throw std::runtime_error("Peer cancelled transfer");continue;}
            cancels=0;if(c==SOH||c==STX||c==EOT)break;
            if(scanned==8191)throw BadFrame();
        }
        if(c==EOT)return {EOT,0,{}};
        // Waiting for a frame and receiving its contents are separate bounded
        // phases. A retransmitted SOH arriving near the wait deadline must not
        // lose its payload to that already-expiring deadline (notably after a
        // lost ACK when both peers' retry timers expire together).
        deadline=Deadline();
        const int sequence=Get(deadline),inverse=Get(deadline);
        Frame frame{c,sequence,Bytes(c==STX?1024:128)};
        for(auto& b:frame.bytes)b=static_cast<std::uint8_t>(Get(deadline));
        bool valid=((sequence+inverse)&255)==255;
        if(crc) {const int hi=Get(deadline),lo=Get(deadline);valid=valid&&(Crc16(frame.bytes.data(),frame.bytes.size())==((hi<<8)|lo));}
        else {unsigned sum=0;for(auto b:frame.bytes)sum+=b;const auto check=Get(deadline);valid=valid&&((sum&255)==static_cast<unsigned>(check));}
        if(!valid)throw BadFrame();
        return frame;
    }
    Bytes Source(std::uint64_t offset,std::size_t count) {
        if(offset>o.size||count>o.size-offset)throw std::runtime_error("Source offset exceeds file snapshot");
        Bytes bytes(count);
        if(count&&io.readFile(offset,bytes.data(),count)!=count)throw std::runtime_error("Source file changed or could not be read");
        return bytes;
    }
    void Progress(std::uint64_t n) {result.bytes=n;if(io.progress)io.progress(n,o.direction==Direction::Send?o.size:(hasDeclared?declared:0));}
    void Store(const Bytes& bytes) {
        if(result.bytes>o.receiveLimit||bytes.size()>o.receiveLimit-result.bytes)throw std::runtime_error("Receive size limit exceeded");
        if(!bytes.empty())io.writeFile(bytes.data(),bytes.size());
        Progress(result.bytes+bytes.size());
    }
    static std::uint64_t Number(const Bytes& bytes,std::size_t start,bool& present) {
        present=false;std::uint64_t value=0;
        for(std::size_t i=start;i<bytes.size()&&bytes[i]&&bytes[i]!=' ';++i) {
            if(bytes[i]<'0'||bytes[i]>'9'||value>(std::numeric_limits<std::uint64_t>::max()-(bytes[i]-'0'))/10)
                throw std::runtime_error("Invalid or overflowing peer file length");
            present=true;value=value*10+(bytes[i]-'0');
        }
        return value;
    }
    void Metadata(const Bytes& bytes) {
        const auto zero=std::find(bytes.begin(),bytes.end(),0);
        if(zero==bytes.end()||zero==bytes.begin())throw std::runtime_error("Peer supplied no file name");
        result.filename=std::string(bytes.begin(),zero);
        if(result.filename.size()>255||result.filename=="."||result.filename==".."||
            result.filename.find_first_of("/\\:")!=std::string::npos||
            std::any_of(result.filename.begin(),result.filename.end(),[](unsigned char c){return c<32||c==127;}))
            throw std::runtime_error("Peer file name is unsafe; select a single ordinary file name");
        declared=Number(bytes,static_cast<std::size_t>(zero-bytes.begin())+1,hasDeclared);
        if(hasDeclared&&declared>o.receiveLimit)throw std::runtime_error("Peer file exceeds receive size limit");
        if(io.metadata)io.metadata(result.filename,declared,hasDeclared);
    }
    Bytes FileInfo(std::size_t length=0) {
        if(o.filename.empty()||o.filename.find_first_of("/\\:\r\n")!=std::string::npos||
            o.filename.find('\0')!=std::string::npos)throw std::runtime_error("Invalid source file name");
        Bytes meta(o.filename.begin(),o.filename.end());meta.push_back(0);
        const auto size=std::to_string(o.size);meta.insert(meta.end(),size.begin(),size.end());meta.push_back(0);
        if(length) {if(meta.size()>length)throw std::runtime_error("File name is too long for YMODEM metadata");meta.resize(length,0);}
        return meta;
    }
    void SendEot() {
        for(unsigned i=0;i<o.retries;++i) {
            Put(EOT);try {if(Control(Deadline())==ACK)return;}catch(const Timeout&) {}
        }
        throw std::runtime_error("End of file was not acknowledged");
    }
    Result SendXY() {
        const bool y=o.protocol==Protocol::Ymodem;
        const bool crc=Handshake();
        if(y) {SendBlock(0,FileInfo(128),crc);Handshake();}
        std::uint64_t offset=0;unsigned sequence=1;
        while(offset<o.size) {
            const std::size_t block=y&&o.size-offset>128?1024:128;
            const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(block,o.size-offset));
            auto bytes=Source(offset,n);bytes.resize(block,0x1a);
            SendBlock(sequence++,bytes,crc);offset+=n;Progress(offset);
        }
        SendEot();
        if(y) {Handshake();SendBlock(0,Bytes(128,0),crc);}
        result.peerAcknowledged=true;result.includesPadding=!y&&(o.size%128!=0);return result;
    }
    Result ReceiveXY() {
        const bool y=o.protocol==Protocol::Ymodem;
        bool crc=true;Frame first;bool ready=false;
        for(unsigned i=0;i<o.retries;++i) {
            if(!y&&i>=3)crc=false;
            Put(crc?CRC:NAK);
            try {first=ReadBlock(crc);ready=true;break;}catch(const Timeout&) {}catch(const BadFrame&) {}
        }
        if(!ready)throw std::runtime_error("Sender did not enter the selected X/YMODEM mode");
        if(y) {
            if(first.kind==EOT||first.sequence!=0)throw BadFrame();
            Metadata(first.bytes);Put(ACK);Put(CRC);ready=false;
        } else {
            hasDeclared=o.expectedReceiveBytes!=0;declared=o.expectedReceiveBytes;
            if(hasDeclared&&declared>o.receiveLimit)throw std::runtime_error("Expected size exceeds receive limit");
            if(io.metadata)io.metadata("",declared,hasDeclared);
        }
        unsigned sequence=1,bad=0;
        for(;;) {
            Frame frame;
            try {frame=ready?std::move(first):ReadBlock(crc);ready=false;}
            catch(const Timeout&) {if(++bad>=o.retries)throw;Put(NAK);continue;}
            catch(const BadFrame&) {if(++bad>=o.retries)throw;Put(NAK);continue;}
            if(frame.kind==EOT) {
                if(hasDeclared&&result.bytes!=declared) {Put(NAK);if(++bad>=o.retries)throw std::runtime_error("Peer ended before the declared file length");continue;}
                if(y) {
                    Put(NAK);bool second=false;
                    for(unsigned i=0;i<o.retries;++i)try {if(ReadBlock(crc).kind==EOT){second=true;break;}Put(NAK);}catch(const Timeout&) {Put(NAK);}
                    if(!second)throw std::runtime_error("Incomplete YMODEM end-of-file handshake");
                }
                Put(ACK);break;
            }
            if(frame.sequence==static_cast<int>((sequence-1)&255)) {
                Put(ACK);if(y&&sequence==1)Put(CRC);continue;
            }
            if(frame.sequence!=static_cast<int>(sequence&255)) {Put(NAK);if(++bad>=o.retries)throw BadFrame();continue;}
            if(hasDeclared) {
                if(result.bytes>=declared)throw std::runtime_error("Peer sent beyond declared file length");
                frame.bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(frame.bytes.size(),declared-result.bytes)));
            }
            Store(frame.bytes);Put(ACK);++sequence;bad=0;
        }
        if(y) {
            bool ended=false;
            for(unsigned i=0;i<o.retries;++i) {
                Put(CRC);
                try {
                    auto tail=ReadBlock(crc);
                    if(tail.kind==EOT){Put(ACK);continue;}
                    if(tail.sequence!=0)throw BadFrame();
                    if(tail.bytes[0])throw std::runtime_error("Select one file per receive operation; a second batch file was refused");
                    Put(ACK);ended=true;break;
                }catch(const Timeout&) {}catch(const BadFrame&) {Put(NAK);}
            }
            if(!ended)throw std::runtime_error("YMODEM batch terminator was not received");
        }
        result.peerAcknowledged=true;result.includesPadding=!y&&!hasDeclared;return result;
    }
    int ZGet(Clock::time_point deadline) {
        for(;;) {const int c=Get(deadline);if(c!=0x11&&c!=0x13&&c!=0x91&&c!=0x93)return c;}
    }
    int ZDecoded(Clock::time_point deadline,bool& end) {
        end=false;const int c=ZGet(deadline);if(c!=ZDLE)return c;
        const int next=ZGet(deadline);
        if(next>=ZCRCE&&next<=ZCRCW) {end=true;return next;}
        if(next=='l')return 127;
        if(next=='m')return 255;
        if((next&0x60)==0x40)return next^0x40;
        if(next==CAN)throw std::runtime_error("Peer cancelled ZMODEM transfer");
        throw BadFrame();
    }
    static int Hex(int c) {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;throw BadFrame();}
    ZHeader ReadHeader() {
        const auto deadline=Deadline();int fmt=0,cancels=0;
        for(unsigned n=0;n<65536;++n) {
            int c=ZGet(deadline);
            if(c==CAN) {if(++cancels>=5)throw std::runtime_error("Peer cancelled ZMODEM transfer");continue;}
            cancels=0;if(c!=ZPAD)continue;
            do {c=ZGet(deadline);}while(c==ZPAD);
            if(c!=ZDLE)continue;
            fmt=ZGet(deadline);if(fmt==ZHEX||fmt==ZBIN||fmt==ZBIN32)break;
            fmt=0;
        }
        if(!fmt)throw BadFrame();
        Bytes bytes(5);bool end=false;
        const auto hexByte=[&](){const int hi=Hex(ZGet(deadline));return (hi<<4)|Hex(ZGet(deadline));};
        for(auto& b:bytes) {const int c=fmt==ZHEX?hexByte():ZDecoded(deadline,end);if(end)throw BadFrame();b=static_cast<std::uint8_t>(c);}
        std::uint32_t expected=0;
        for(int i=0;i<(fmt==ZBIN32?4:2);++i) {
            const int c=fmt==ZHEX?hexByte():ZDecoded(deadline,end);if(end)throw BadFrame();
            if(fmt==ZBIN32)expected|=static_cast<std::uint32_t>(c)<<(i*8);else expected=(expected<<8)|c;
        }
        if(expected!=(fmt==ZBIN32?Crc32(bytes.data(),bytes.size()):Crc16(bytes.data(),bytes.size())))throw BadFrame();
        if(fmt==ZHEX) {
            for(int i=0;i<2;++i) {const int c=Get(deadline);if(c!='\r'&&c!='\n'&&c!=0x8a){unread.push_back(c);break;}}
        }
        return {bytes[0],static_cast<std::uint32_t>(bytes[1])|(static_cast<std::uint32_t>(bytes[2])<<8)|
                        (static_cast<std::uint32_t>(bytes[3])<<16)|(static_cast<std::uint32_t>(bytes[4])<<24),fmt==ZBIN32};
    }
    void Header(int type,std::uint32_t position=0) {
        Bytes bytes{static_cast<std::uint8_t>(type),static_cast<std::uint8_t>(position),static_cast<std::uint8_t>(position>>8),static_cast<std::uint8_t>(position>>16),static_cast<std::uint8_t>(position>>24)};
        const auto crc=Crc16(bytes.data(),bytes.size());bytes.push_back(static_cast<std::uint8_t>(crc>>8));bytes.push_back(static_cast<std::uint8_t>(crc&255));
        Bytes out{'*','*',ZDLE,ZHEX};const char* hex="0123456789abcdef";
        for(auto b:bytes){out.push_back(hex[b>>4]);out.push_back(hex[b&15]);}
        out.push_back('\r');out.push_back('\n');if(type!=ZFIN&&type!=ZACK)out.push_back(0x11);Put(out);
    }
    static void Escape(Bytes& out,int c) {
        if(c==127){out.push_back(ZDLE);out.push_back('l');}
        else if(c==255){out.push_back(ZDLE);out.push_back('m');}
        else if((c&0x60)==0){out.push_back(ZDLE);out.push_back(c^0x40);}
        else out.push_back(static_cast<std::uint8_t>(c));
    }
    void Packet(const Bytes& bytes,int end) {
        Bytes check=bytes;check.push_back(static_cast<std::uint8_t>(end));
        const auto crc=Crc16(check.data(),check.size());Bytes out;out.reserve(bytes.size()*2+8);
        for(auto b:bytes)Escape(out,b);
        out.push_back(ZDLE);out.push_back(static_cast<std::uint8_t>(end));
        Escape(out,crc>>8);Escape(out,crc&255);if(end==ZCRCW)out.push_back(0x11);Put(out);
    }
    ZPacket ReadPacket(bool crc32) {
        const auto deadline=Deadline();ZPacket packet;bool end=false;
        while(packet.bytes.size()<=8192) {
            const int c=ZDecoded(deadline,end);if(end){packet.end=c;break;}
            packet.bytes.push_back(static_cast<std::uint8_t>(c));
        }
        if(!packet.end)throw std::runtime_error("ZMODEM subpacket exceeds 8192-byte bound");
        Bytes check=packet.bytes;check.push_back(static_cast<std::uint8_t>(packet.end));std::uint32_t expected=0;
        for(int i=0;i<(crc32?4:2);++i) {
            const int c=ZDecoded(deadline,end);if(end)throw BadFrame();
            if(crc32)expected|=static_cast<std::uint32_t>(c)<<(8*i);else expected=(expected<<8)|c;
        }
        if(expected!=(crc32?Crc32(check.data(),check.size()):Crc16(check.data(),check.size())))throw BadFrame();
        return packet;
    }
    static void PeerFailure(const ZHeader& h) {
        if(h.type==ZSKIP||h.type==ZABORT||h.type==ZFERR)throw std::runtime_error("Peer refused or aborted ZMODEM file");
    }
    Result SendZ() {
        if(o.size>0xffffffffull)throw std::runtime_error("ZMODEM uses 32-bit offsets; file must be smaller than 4 GiB");
        bool ready=false;
        for(unsigned i=0;i<o.retries;++i) {
            Header(ZRQINIT);
            try {const auto h=ReadHeader();PeerFailure(h);if(h.type==ZRINIT){ready=true;break;}}catch(const Timeout&) {}catch(const BadFrame&) {}
        }
        if(!ready)throw std::runtime_error("Receiver did not enter ZMODEM mode");
        std::uint64_t offset=0;ready=false;
        for(unsigned i=0;i<o.retries;++i) {
            Header(ZFILE,1u<<24);Packet(FileInfo(),ZCRCW);
            try {
                // A receiver can announce ZRINIT before our ZRQINIT and answer
                // that request again. Consume the old announcement instead of
                // resending ZFILE and creating an escalating response backlog.
                for(unsigned pending=0;pending<16;++pending) {
                    const auto h=ReadHeader();PeerFailure(h);
                    if(h.type==ZRPOS){offset=h.position;ready=true;break;}
                    if(h.type!=ZRINIT&&h.type!=ZACK)break;
                }
                if(ready)break;
            }catch(const Timeout&) {}catch(const BadFrame&) {}
        }
        if(!ready||offset>o.size)throw std::runtime_error("Peer supplied no valid ZMODEM file position");
        unsigned failures=0;
        for(;;) {
            Check();
            if(offset<o.size) {
                const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(1024,o.size-offset));
                Header(ZDATA,static_cast<std::uint32_t>(offset));Packet(Source(offset,n),ZCRCW);
                try {
                    const auto h=ReadHeader();PeerFailure(h);
                    if(h.type==ZACK&&h.position==offset+n){offset+=n;Progress(offset);failures=0;continue;}
                    if(h.type==ZRPOS&&h.position<=o.size){if(h.position<=offset&&++failures>=o.retries)throw std::runtime_error("Repeated ZMODEM retransmission limit");offset=h.position;Progress(offset);continue;}
                }catch(const Timeout&) {}catch(const BadFrame&) {}
                if(++failures>=o.retries)throw std::runtime_error("ZMODEM data acknowledgement timeout");
            } else {
                Header(ZEOF,static_cast<std::uint32_t>(offset));
                try {
                    const auto h=ReadHeader();PeerFailure(h);
                    if(h.type==ZRINIT)break;
                    if(h.type==ZRPOS&&h.position<=o.size){offset=h.position;if(++failures>=o.retries)throw std::runtime_error("ZMODEM end-of-file retry limit");continue;}
                }catch(const Timeout&) {}catch(const BadFrame&) {}
                if(++failures>=o.retries)throw std::runtime_error("ZMODEM end of file was not acknowledged");
            }
        }
        for(unsigned i=0;i<o.retries;++i) {
            Header(ZFIN);
            try {const auto h=ReadHeader();PeerFailure(h);if(h.type==ZFIN){Put(Bytes{'O','O'});result.peerAcknowledged=true;return result;}}catch(const Timeout&) {}catch(const BadFrame&) {}
        }
        throw std::runtime_error("ZMODEM finish handshake incomplete");
    }
    Result ReceiveZ() {
        bool opened=false,ended=false;unsigned failures=0;
        Header(ZRINIT,0x63000000u);
        for(;;) {
            ZHeader h;
            try {h=ReadHeader();}catch(const Timeout&) {
                if(++failures>=o.retries)throw;
                Header(opened&&!ended?ZRPOS:ZRINIT,opened&&!ended?static_cast<std::uint32_t>(result.bytes):0x63000000u);continue;
            }catch(const BadFrame&) {
                if(++failures>=o.retries)throw;
                Header(opened&&!ended?ZRPOS:ZNAK,static_cast<std::uint32_t>(result.bytes));continue;
            }
            PeerFailure(h);
            if(h.type==ZRQINIT){Header(ZRINIT,0x63000000u);continue;}
            if(h.type==ZSINIT) {try {ReadPacket(h.crc32);Header(ZACK,1);}catch(const BadFrame&) {Header(ZNAK);}continue;}
            if(h.type==ZFILE) {
                const auto meta=ReadPacket(h.crc32);
                if(ended)throw std::runtime_error("Select one file per receive operation; second batch file refused");
                if(!opened){Metadata(meta.bytes);if(hasDeclared&&declared>0xffffffffull)throw std::runtime_error("ZMODEM file exceeds 32-bit offsets");opened=true;}
                Header(ZRPOS,static_cast<std::uint32_t>(result.bytes));continue;
            }
            if(h.type==ZDATA&&opened&&!ended) {
                const bool matching=h.position==result.bytes;
                bool responded=false;
                try {
                    for(;;) {
                        auto packet=ReadPacket(h.crc32);
                        if(matching) {
                            if(hasDeclared&&(result.bytes>declared||packet.bytes.size()>declared-result.bytes))throw std::runtime_error("Peer sent beyond declared file length");
                            if(result.bytes>0xffffffffull||packet.bytes.size()>0xffffffffull-result.bytes)throw std::runtime_error("ZMODEM offset overflow");
                            Store(packet.bytes);failures=0;
                        }
                        if(packet.end==ZCRCW||packet.end==ZCRCQ) {Header(matching?ZACK:ZRPOS,static_cast<std::uint32_t>(result.bytes));responded=true;}
                        if(packet.end==ZCRCE||packet.end==ZCRCW)break;
                    }
                    if(!matching&&!responded)Header(ZRPOS,static_cast<std::uint32_t>(result.bytes));
                }catch(const BadFrame&) {if(++failures>=o.retries)throw;Header(ZRPOS,static_cast<std::uint32_t>(result.bytes));}
                 catch(const Timeout&) {if(++failures>=o.retries)throw;Header(ZRPOS,static_cast<std::uint32_t>(result.bytes));}
                continue;
            }
            if(h.type==ZEOF&&opened) {
                if(h.position!=result.bytes||(hasDeclared&&declared!=result.bytes)) {
                    if(++failures>=o.retries)throw std::runtime_error("ZMODEM peer ended at an incorrect file position");
                    Header(ZRPOS,static_cast<std::uint32_t>(result.bytes));continue;
                }
                ended=true;Header(ZRINIT,0x63000000u);continue;
            }
            if(h.type==ZFIN&&ended) {
                for(unsigned i=0;i<o.retries;++i) {
                    Header(ZFIN);
                    try {
                        const auto deadline=Deadline();const int c=ZGet(deadline);
                        if(c=='O'&&ZGet(deadline)=='O'){result.peerAcknowledged=true;return result;}
                        unread.push_back(c);const auto again=ReadHeader();if(again.type!=ZFIN)PeerFailure(again);
                    }catch(const Timeout&) {}catch(const BadFrame&) {}
                }
                throw std::runtime_error("ZMODEM final OO acknowledgement missing");
            }
            if(++failures>=o.retries)throw std::runtime_error("Unexpected ZMODEM state");
            Header(opened&&!ended?ZRPOS:ZRINIT,opened&&!ended?static_cast<std::uint32_t>(result.bytes):0x63000000u);
        }
    }
    Result Raw() {
        if(o.direction==Direction::Send) {
            for(std::uint64_t offset=0;offset<o.size;) {
                auto bytes=Source(offset,static_cast<std::size_t>(std::min<std::uint64_t>(4096,o.size-offset)));
                Put(bytes);offset+=bytes.size();Progress(offset);
            }
        } else {
            hasDeclared=o.expectedReceiveBytes!=0;declared=o.expectedReceiveBytes;
            if(hasDeclared&&declared>o.receiveLimit)throw std::runtime_error("Expected size exceeds receive limit");
            if(io.metadata)io.metadata("",declared,hasDeclared);
            Bytes bytes;bytes.reserve(4096);
            auto last=Clock::now();
            while(!hasDeclared||result.bytes+bytes.size()<declared) {
                Check();
                if(io.finishRaw&&io.finishRaw())break;
                const int c=io.readByte(50);
                if(c>=0){bytes.push_back(static_cast<std::uint8_t>(c));last=Clock::now();}
                if(bytes.size()==4096||(c<0&&!bytes.empty())){Store(bytes);bytes.clear();}
                if(c<0&&hasDeclared&&Clock::now()-last>std::chrono::milliseconds(o.timeoutMs))throw Timeout();
            }
            Store(bytes);
            if(hasDeclared&&result.bytes!=declared)throw std::runtime_error("Raw receive ended before expected byte count");
        }
        return result;
    }
  public:
    Engine(const Options& options,const Io& functions):o(options),io(functions){result.filename=o.filename;}
    Result Execute() {
        if(o.timeoutMs<50||o.timeoutMs>120000||!o.retries||o.retries>100)throw std::runtime_error("Invalid protocol timeout/retry configuration");
        Check();
        if(o.protocol==Protocol::Raw)return Raw();
        if(o.protocol==Protocol::Zmodem)return o.direction==Direction::Send?SendZ():ReceiveZ();
        return o.direction==Direction::Send?SendXY():ReceiveXY();
    }
};
} // namespace
Result Run(const Options& options,const Io& io) {
    try {return Engine(options,io).Execute();}
    catch(...) {if(options.protocol!=Protocol::Raw&&io.abortWire)try {io.abortWire();}catch(...) {}throw;}
}
} // namespace serialctl::xfer


