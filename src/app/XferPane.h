#pragma once
#include "TransferController.h"
#include "UiStyle.h"

namespace serialctl {
class XferPane {
  public:
    struct Target {std::uint64_t id=0;std::wstring name,unavailable;std::shared_ptr<QueuedConnection> connection;};
    enum ControlId {Direction=10,Mode,File,Browse,Length,Start,Cancel,Help,Status,Source,Count,
                    DirectionLabel,ModeLabel,FileLabel,LengthLabel};
    ~XferPane();
    bool Create(HWND,xfer::TransferController&,std::function<Target()>,HFONT,bool,int);
    HWND Handle()const{return window_;}
    void Theme(bool,int,HFONT);
    void Refresh();
    std::function<void()> stateChanged;
  private:
    static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK BodyProc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK FocusProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    HWND Control(int id)const;
    void Layout();
    void Scroll(int);
    void BrowseFile();
    void Begin();
    void Paint(HDC);
    int D(int n)const{return MulDiv(n,dpi_,96);}
    HWND window_=nullptr,body_=nullptr;
    HFONT font_=nullptr;
    HBRUSH surface_=nullptr,field_=nullptr;
    bool dark_=false,wasBusy_=false,directionChanged_=false,dragging_=false;
    int grab_=0;
    std::wstring localNotice_;
    int dpi_=96,offset_=0,contentHeight_=0,bodyHeight_=0,progressTop_=0;
    std::uint64_t displayBytes_=0,displayTotal_=0;
    UiFieldStyle style_{};
    xfer::TransferController* controller_=nullptr;
    std::function<Target()> target_;
};
} // namespace serialctl
