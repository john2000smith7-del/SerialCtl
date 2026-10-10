#pragma once
#include <algorithm>
#include <string>
#include <vector>
namespace serialctl {
// Local CMD drafts never enter stdout/VT or the network until submitted.
class CmdLineEditor {
public:
    enum class Key {Left,Right,Home,End,Backspace,Delete,Up,Down};
    const std::wstring& Text()const{return text_;}
    size_t Cursor()const{return cursor_;}
    bool Insert(const std::wstring& text){if(text_.size()+text.size()>8191)return false;text_.insert(cursor_,text);cursor_+=text.size();return true;}
    void Edit(Key key){switch(key){
        case Key::Left:if(cursor_){--cursor_;if(cursor_&&Low(text_[cursor_]))--cursor_;}break;
        case Key::Right:if(cursor_<text_.size()){if(High(text_[cursor_])&&cursor_+1<text_.size())++cursor_;++cursor_;}break;
        case Key::Home:cursor_=0;break;case Key::End:cursor_=text_.size();break;
        case Key::Backspace:if(cursor_){auto end=cursor_;Edit(Key::Left);text_.erase(cursor_,end-cursor_);}break;
        case Key::Delete:if(cursor_<text_.size()){auto begin=cursor_;Edit(Key::Right);text_.erase(begin,cursor_-begin);cursor_=begin;}break;
        case Key::Up:if(!history_.empty()){if(historyIndex_==history_.size())draft_=text_;if(historyIndex_)--historyIndex_;text_=history_[historyIndex_];cursor_=text_.size();}break;
        case Key::Down:if(historyIndex_<history_.size()){++historyIndex_;text_=historyIndex_==history_.size()?draft_:history_[historyIndex_];cursor_=text_.size();}break;
    }}
    void Submitted(){if(!text_.empty()&&(history_.empty()||history_.back()!=text_)){history_.push_back(text_);if(history_.size()>128)history_.erase(history_.begin());}Clear();}
    void Clear(){text_.clear();draft_.clear();cursor_=0;historyIndex_=history_.size();}
private:
    static bool High(wchar_t c){return c>=0xd800&&c<=0xdbff;}
    static bool Low(wchar_t c){return c>=0xdc00&&c<=0xdfff;}
    std::wstring text_,draft_;
    size_t cursor_=0,historyIndex_=0;
    std::vector<std::wstring> history_;
};
} // namespace serialctl
