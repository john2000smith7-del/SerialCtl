#include "TransferController.h"
#include "Win32Helpers.h"
#include "FilePaths.h"
#include <condition_variable>
#include <deque>
#include <cstring>

namespace serialctl::xfer {
struct TransferController::Task {
    Request request;
    std::string owner;
    std::atomic_bool cancel{false},finish{false},done{false},success{false},cancelled{false},overflow{false};
    std::atomic_bool peerAcknowledged{false},includesPadding{false};
    std::atomic_uint64_t bytes{0},total{0};
    std::atomic_uint64_t acceptedReceive{0},rawCutoff{0};
    std::size_t finishRemaining=0;
    std::mutex textMutex,rxMutex;
    std::condition_variable wake;
    std::deque<std::uint8_t> rx;
    std::wstring note=L"正在准备文件与串口…",partial,peerName;
    HANDLE workerHandle=nullptr;
    bool restoreTail=false;
    ~Task(){if(workerHandle)CloseHandle(workerHandle);}
    void Note(std::wstring text){std::lock_guard<std::mutex> lock(textMutex);note=std::move(text);}
    void Offer(const serialctl::Bytes& bytes) noexcept {
        try {
            std::lock_guard<std::mutex> lock(rxMutex);
            if(bytes.size()>4*1024*1024-rx.size())overflow=true;
            else {rx.insert(rx.end(),bytes.begin(),bytes.end());acceptedReceive+=bytes.size();}
            wake.notify_all();
        }catch(...) {overflow=true;wake.notify_all();}
    }
    serialctl::Bytes Drain() {
        std::lock_guard<std::mutex> lock(rxMutex);
        serialctl::Bytes tail;
        if(restoreTail)tail.assign(rx.begin(),rx.end());
        rx.clear();return tail;
    }
};
namespace {
struct File {
    HANDLE handle=INVALID_HANDLE_VALUE;
    ~File(){Close();}
    void Close(){if(handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;}}
};
void Failure(const char* action) {
    const auto message=Win32ErrorMessage();
    throw std::runtime_error(std::string(action)+": "+WideToMultiByte(message,CP_UTF8));
}
void Check(const TransferController::Request& request) {
    if(!request.session||!request.connection||request.path.empty())throw std::runtime_error("Invalid transfer target or file path");
}
}
TransferController::~TransferController(){Shutdown();}
bool TransferController::Start(Request request,std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(task_&&!task_->done){error=L"已有文件传输任务，请先完成或取消";return false;}
    if(worker_.joinable())worker_.join(); // done is published only after cleanup/file handles close.
    if(!request.connection||!request.connection->IsConnected()||!request.connection->SupportsTransfer()) {
        error=L"当前连接未就绪或远端版本不支持 Xfer";return false;
    }
    try {
        auto task=std::make_shared<Task>();task->request=std::move(request);
        task->owner="xfer-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(next_++);
        task_=task;worker_=std::thread(Work,task);
        HANDLE duplicate=nullptr;
        if(DuplicateHandle(GetCurrentProcess(),reinterpret_cast<HANDLE>(worker_.native_handle()),GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))
            task->workerHandle=duplicate;
        return true;
    }catch(const std::exception&) {error=L"无法启动文件传输任务";if(task_)task_->done=true;return false;}
}
TransferController::Snapshot TransferController::State() const {
    std::shared_ptr<Task> task;
    {std::lock_guard<std::mutex> lock(mutex_);task=task_;}
    Snapshot state;if(!task)return state;
    state.session=task->request.session;state.source=task->request.source;state.path=task->request.path;
    state.protocol=task->request.options.protocol;state.direction=task->request.options.direction;
    state.busy=!task->done;state.success=task->success;state.cancelled=task->cancelled;
    state.bytes=task->bytes;state.total=task->total;state.peerAcknowledged=task->peerAcknowledged;state.includesPadding=task->includesPadding;
    state.expectedReceiveBytes=task->request.options.expectedReceiveBytes;state.stopping=task->cancel;state.finishing=task->finish;
    state.rawReceiveCutoff=task->rawCutoff;
    {std::lock_guard<std::mutex> lock(task->textMutex);state.note=task->note;state.partial=task->partial;state.peerName=task->peerName;}
    return state;
}
bool TransferController::Busy() const {
    std::lock_guard<std::mutex> lock(mutex_);return task_&&!task_->done;
}
void TransferController::Cancel() {
    std::shared_ptr<Task> task;
    {std::lock_guard<std::mutex> lock(mutex_);task=task_;}
    if(!task||task->done)return;
    task->cancel=true;task->wake.notify_all();
    task->request.connection->CancelTransfer(task->owner);
    if(task->workerHandle)CancelSynchronousIo(task->workerHandle);
}
void TransferController::CancelSession(std::uint64_t id) {
    {std::lock_guard<std::mutex> lock(mutex_);if(!task_||task_->request.session!=id)return;}
    Cancel();
}
void TransferController::FinishRawReceive() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(task_&&task_->request.options.protocol==Protocol::Raw&&task_->request.options.direction==Direction::Receive&&
        !task_->request.options.expectedReceiveBytes&&!task_->finish) {
        std::lock_guard<std::mutex> receive(task_->rxMutex);
        task_->finishRemaining=task_->rx.size();task_->rawCutoff=task_->acceptedReceive.load();
        task_->finish=true;task_->wake.notify_all();
    }
}
void TransferController::Shutdown() {
    Cancel();
    // Shutdown is called once by the owner after GUI tasks have stopped.
    if(worker_.joinable())worker_.join();
}
void TransferController::Work(const std::shared_ptr<Task>& task) {
    bool leased=false,protocolDone=false,receiveFile=false;
    File file;
    std::wstring temporary;
    const auto release=[&] {
        if(!leased)return true;
        std::wstring error;
        const bool result=task->request.connection->EndTransfer(task->owner,error);
        leased=false;
        if(!result)task->Note(L"传输数据阶段结束，但释放串口失败："+error);
        return result;
    };
    try {
        Check(task->request);
        auto options=task->request.options;
        const auto path=ExtendedFilePath(task->request.path);
        if(options.direction==Direction::Send) {
            file.handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
            if(file.handle==INVALID_HANDLE_VALUE)Failure("Cannot open source file");
            LARGE_INTEGER size{};if(!GetFileSizeEx(file.handle,&size)||size.QuadPart<0)Failure("Cannot snapshot source size");
            options.size=static_cast<std::uint64_t>(size.QuadPart);task->total=options.size;
            const auto slash=task->request.path.find_last_of(L"/\\");
            const auto name=slash==std::wstring::npos?task->request.path:task->request.path.substr(slash+1);
            options.filename=WideToMultiByte(name,CP_UTF8);
        } else {
            receiveFile=true;
            const auto attributes=GetFileAttributesW(path.c_str());
            if(attributes!=INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Destination exists; choose a new file name (existing files are never overwritten)");
            const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)Failure("Cannot inspect receive destination");
            temporary=task->request.path+L".serialctl-part-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
            file.handle=CreateFileW(ExtendedFilePath(temporary).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file.handle==INVALID_HANDLE_VALUE)Failure("Cannot create partial receive file");
            {std::lock_guard<std::mutex> lock(task->textMutex);task->partial=temporary;}
        }
        if(task->cancel)throw Cancelled();
        const std::weak_ptr<Task> weak=task;
        std::wstring error;
        if(!task->request.connection->BeginTransfer(task->owner,[weak](const serialctl::Bytes& bytes){if(auto t=weak.lock())t->Offer(bytes);},error,
            [weak]{if(auto t=weak.lock())return t->Drain();return serialctl::Bytes{};}))
            throw std::runtime_error(WideToMultiByte(error,CP_UTF8));
        leased=true;
        task->Note(options.direction==Direction::Send?L"正在发送；等待对端协议反馈…":L"正在接收；请在设备端启动对应发送命令");
        Io io;
        io.cancelled=[task]{return task->cancel.load();};
        io.finishRaw=[task]{std::lock_guard<std::mutex> lock(task->rxMutex);return task->finish&&!task->finishRemaining;};
        io.readByte=[task](unsigned ms) {
            std::unique_lock<std::mutex> lock(task->rxMutex);
            task->wake.wait_for(lock,std::chrono::milliseconds(ms),[&]{return !task->rx.empty()||task->cancel||task->overflow||task->finish;});
            if(task->overflow)throw std::runtime_error("Receive buffer exceeded 4 MiB; transfer aborted without claiming success");
            if(task->cancel)throw Cancelled();
            if(!task->request.connection->IsConnected())throw std::runtime_error("Serial connection was lost");
            if(task->finish&&!task->finishRemaining)return -1;
            if(task->rx.empty())return -1;
            const int byte=task->rx.front();task->rx.pop_front();if(task->finish&&task->finishRemaining)--task->finishRemaining;return byte;
        };
        io.write=[task](const Bytes& bytes) {
            // Short bounded writes keep cancellation opportunities even at low baud.
            for(std::size_t offset=0;offset<bytes.size();offset+=256) {
                if(task->cancel)throw Cancelled();
                serialctl::Bytes chunk(bytes.begin()+offset,bytes.begin()+std::min(bytes.size(),offset+256));
                std::wstring error;
                if(!task->request.connection->SendTransfer(task->owner,chunk,error))
                    throw std::runtime_error(WideToMultiByte(error,CP_UTF8));
            }
        };
        io.abortWire=[task]{task->request.connection->AbortTransfer(task->owner);};
        io.readFile=[&file](std::uint64_t offset,std::uint8_t* bytes,std::size_t size) {
            LARGE_INTEGER position{};position.QuadPart=static_cast<LONGLONG>(offset);
            if(!SetFilePointerEx(file.handle,position,nullptr,FILE_BEGIN))Failure("Cannot seek source file");
            DWORD got=0;if(!ReadFile(file.handle,bytes,static_cast<DWORD>(size),&got,nullptr))Failure("Cannot read source file");
            return static_cast<std::size_t>(got);
        };
        io.writeFile=[&file,task](const std::uint8_t* bytes,std::size_t size) {
            for(std::size_t offset=0;offset<size;) {
                if(task->cancel)throw Cancelled();
                DWORD written=0;
                if(!WriteFile(file.handle,bytes+offset,static_cast<DWORD>(size-offset),&written,nullptr)||!written)Failure("Cannot save received bytes");
                offset+=written;
            }
        };
        io.metadata=[task](const std::string& name,std::uint64_t size,bool known) {
            task->total=known?size:0;
            if(!name.empty()){std::lock_guard<std::mutex> lock(task->textMutex);task->peerName=MultiByteToWide(reinterpret_cast<const std::uint8_t*>(name.data()),name.size(),CP_UTF8);}
        };
        io.progress=[task](std::uint64_t bytes,std::uint64_t total){task->bytes=bytes;task->total=total;};
        const auto result=Run(options,io);
        task->peerAcknowledged=result.peerAcknowledged;task->includesPadding=result.includesPadding;
        protocolDone=true;
        if(task->cancel)throw Cancelled();
        if(receiveFile) {
            task->Note(L"数据已接收，正在刷新并保存文件…");
            if(!FlushFileBuffers(file.handle))Failure("Cannot flush received file");
            file.Close();
            if(task->cancel)throw Cancelled();
            if(!MoveFileExW(ExtendedFilePath(temporary).c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH))
                Failure("Verified receive data retained in partial file; cannot commit destination");
            temporary.clear();
            {std::lock_guard<std::mutex> lock(task->textMutex);task->partial.clear();}
        }
        task->restoreTail=true;
        const bool released=release();
        file.Close();
        if(released) {
            task->success=true;
            if(options.protocol==Protocol::Raw)task->Note(receiveFile?L"原始数据已保存；此模式没有对端校验":L"原始字节已写入传输层；此模式没有对端校验");
            else task->Note(result.includesPadding?L"协议已完成；XMODEM 没有文件长度，接收端可能保留补齐字节":L"传输协议已完成，对端握手已确认");
        }
    }catch(const Cancelled&) {
        task->cancelled=true;
        task->Note(protocolDone?L"数据协议阶段已结束；本地保存/收尾已取消":L"已取消；已经写出的字节无法撤回");
    }catch(const std::exception& e) {
        const auto text=MultiByteToWide(reinterpret_cast<const std::uint8_t*>(e.what()),strlen(e.what()),CP_UTF8);
        if(task->cancel){task->cancelled=true;task->Note(protocolDone?L"数据协议阶段已结束；本地保存/收尾已取消":L"已取消；已经写出的字节无法撤回");}
        else task->Note((protocolDone?L"数据协议阶段已结束，但本地保存/收尾失败：":L"传输失败：")+text);
    }catch(...) {task->Note(L"传输失败：未预期的内部错误");}
    try {release();}catch(...) {task->Note(L"传输结束，但串口释放失败");}
    file.Close();
    if(!temporary.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA partial{};
        if(GetFileAttributesExW(ExtendedFilePath(temporary).c_str(),GetFileExInfoStandard,&partial)&&!partial.nFileSizeHigh&&!partial.nFileSizeLow&&
           DeleteFileW(ExtendedFilePath(temporary).c_str())) {
            std::lock_guard<std::mutex> lock(task->textMutex);task->partial.clear();
        }
    }
    task->done=true;task->wake.notify_all();
}
} // namespace serialctl::xfer

