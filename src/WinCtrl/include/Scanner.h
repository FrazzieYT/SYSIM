#pragma once
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <string>

#pragma comment(lib, "wintrust.lib")

namespace WinCtrl::Scanner {
struct ScanResult {
    enum class SigStatus { Unknown, Valid, Unsigned, Invalid, Error };
};

inline ScanResult::SigStatus VerifySignature(const std::wstring& path,
    std::wstring* signer, void*, bool* isMicrosoft, bool* isTrusted, bool) {
    if (signer) signer->clear();
    if (isMicrosoft) *isMicrosoft = false;
    if (isTrusted) *isTrusted = false;
    WINTRUST_FILE_INFO file{ sizeof(file) };
    file.pcwszFilePath = path.c_str();
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{ sizeof(data) };
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG status = WinVerifyTrust(nullptr, &policy, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &data);
    if (status == ERROR_SUCCESS) {
        if (isTrusted) *isTrusted = true;
        if (isMicrosoft && path.find(L"\\Windows\\") != std::wstring::npos)
            *isMicrosoft = true;
        return ScanResult::SigStatus::Valid;
    }
    return ScanResult::SigStatus::Invalid;
}
}
