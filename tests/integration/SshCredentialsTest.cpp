#include "SshCredentials.h"
#include "SshConnectionHistory.h"
#include <iostream>
#include <stdexcept>

using namespace serialctl;
static void Require(bool value,const char* note) {if(!value)throw std::runtime_error(note);}
int main() {
    try {
        const auto host=L"serialctl-credential-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        const auto target=SshCredentialTarget(host,22,L"fixture");
        struct Cleanup {std::wstring target;~Cleanup(){DWORD error{};ForgetSshCredential(target,error);}} cleanup{target};
        Require(SshCredentialTarget(L"EXAMPLE",22,L"user")==SshCredentialTarget(L"example",22,L"user"),"host case must match");
        Require(SshCredentialTarget(L"example",22,L"User")!=SshCredentialTarget(L"example",22,L"user"),"account case must remain distinct");
        Require(SshCredentialTarget(host,2222,L"fixture")!=target,"ports must not share passwords");
        Require(SshCredentialTarget(L"a/22/1:b",22,L"c")!=SshCredentialTarget(L"a",22,L"b/22/1:c"),"delimiter aliases");
        DWORD error{};std::wstring password=L"stale";
        Require(ReadSshCredential(target,password,error)==SshCredentialResult::Missing&&password.empty(),"missing entry must clear old target password");
        const std::wstring secret=L"fixture-\x5bc6\x7801-\x03bb";
        Require(SaveSshCredential(target,L"fixture",secret,error),"save per-user credential");
        Require(ReadSshCredential(target,password,error)==SshCredentialResult::Loaded&&password==secret,"Unicode roundtrip after independent read");
        const auto other=SshCredentialTarget(host,2222,L"fixture");
        Require(ReadSshCredential(other,password,error)==SshCredentialResult::Missing&&password.empty(),"new target must not inherit previous password");
        Require(SaveSshCredential(target,L"fixture",L"changed",error),"replace credential after successful login");
        Require(ReadSshCredential(target,password,error)==SshCredentialResult::Loaded&&password==L"changed","replacement roundtrip");
        Require(!SaveSshCredential(target,L"fixture",std::wstring(CRED_MAX_CREDENTIAL_BLOB_SIZE,L'x'),error)&&error==ERROR_BAD_LENGTH,"reject oversized secret without truncation");
        Require(ForgetSshCredential(target,error)&&ForgetSshCredential(target,error),"forget is idempotent");
        Require(ReadSshCredential(target,password,error)==SshCredentialResult::Missing&&password.empty(),"forgotten password cannot refill");
        const auto history=NormalizeSshHistory(nlohmann::json::array({{{"host","example"},{"port",22u},{"username","fixture"},{"password","must-not-persist"}}}));
        Require(history.size()==1&&!history[0].contains("password"),"history must never serialize passwords");
        auto mutableHistory = history;
        const auto histTarget = SshCredentialTarget(L"example", 22, L"fixture");
        Require(SaveSshCredential(histTarget, L"fixture", L"histSecret", error), "save hist credential");
        Require(ReadSshCredential(histTarget, password, error) == SshCredentialResult::Loaded, "read hist credential");
        Require(ForgetSshCredential(histTarget, error), "forget hist credential on history deletion");
        mutableHistory.clear();
        Require(mutableHistory.empty(), "history cleared");
        Require(ReadSshCredential(histTarget, password, error) == SshCredentialResult::Missing, "credential forgotten after history deletion");
        ClearSshSecret(password);
        std::cout<<"PASS: isolated Windows SSH credential save/read/update/forget and history isolation\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
