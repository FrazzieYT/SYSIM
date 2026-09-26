#include "text_file.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace TextFile {

    static const size_t MAX_LOAD_SIZE = 16 * 1024 * 1024; // 16 МБ

#ifndef ERROR_FILE_TOO_LARGE
#define ERROR_FILE_TOO_LARGE 0x000000DA
#endif

    // UTF-8 validation
    static bool IsValidUtf8(const BYTE* data, size_t size, bool& hasNonAscii) {
        hasNonAscii = false;
        size_t i = 0;

        while (i < size) {
            BYTE b = data[i];
            size_t extra;

            if (b < 0x80) {
                ++i;
                continue;
            }
            else if (b >= 0xC2 && b <= 0xDF) extra = 1;
            else if (b >= 0xE0 && b <= 0xEF) extra = 2;
            else if (b >= 0xF0 && b <= 0xF4) extra = 3;
            else return false;

            if (i + extra >= size) return false;

            for (size_t k = 1; k <= extra; ++k) {
                BYTE c = data[i + k];
                if (c < 0x80 || c > 0xBF) return false;
            }

            hasNonAscii = true;
            i += extra + 1;
        }

        return true;
    }
    
    // Encoding detection
    Encoding DetectEncoding(const std::vector<BYTE>& bytes) {
        const size_t n = bytes.size();
        const BYTE* p = bytes.data();

        // BOM
        if (n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0x00 && p[3] == 0x00)
            return Encoding::Utf32LE;
        if (n >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFE && p[3] == 0xFF)
            return Encoding::Utf32BE;
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE)
            return Encoding::Utf16LE;
        if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF)
            return Encoding::Utf16BE;
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
            return Encoding::Utf8Bom;

        if (n == 0)
            return Encoding::Ansi;

        // UTF-16 no-BOM heuristics: multiple null bytes
        size_t zerosOdd = 0;
        size_t zerosEven = 0;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] == 0) {
                if (i & 1) ++zerosOdd;
                else ++zerosEven;
            }
        }

        if ((zerosOdd + zerosEven) > 0 && (zerosOdd + zerosEven) * 4 >= n) {
            return (zerosOdd >= zerosEven)
                ? Encoding::Utf16LE
                : Encoding::Utf16BE;
        }

        // UTF-8 no-BOM or ANSI
        bool hasNonAscii = false;
        if (IsValidUtf8(p, n, hasNonAscii) && hasNonAscii) {
            return Encoding::Utf8;
        }

        return Encoding::Ansi;
    }
    
    // Transformations
    static void AppendCodePoint(std::wstring& s, uint32_t cp) {
        if (cp <= 0xFFFF) {
            s.push_back(static_cast<wchar_t>(cp));
        }
        else if (cp <= 0x10FFFF) {
            cp -= 0x10000;
            s.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            s.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
        else {
            s.push_back(L'?');
        }
    }

    static std::vector<BYTE> WStringToUtf32(const std::wstring& text, bool littleEndian) {
        std::vector<BYTE> buf;
        buf.reserve(text.size() * 4);

        for (size_t i = 0; i < text.size(); ++i) {
            uint32_t cp = text[i];

            // Surrogate pair
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
                wchar_t next = text[i + 1];
                if (next >= 0xDC00 && next <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (next - 0xDC00);
                    ++i;
                }
            }

            if (littleEndian) {
                buf.push_back(cp & 0xFF);
                buf.push_back((cp >> 8) & 0xFF);
                buf.push_back((cp >> 16) & 0xFF);
                buf.push_back((cp >> 24) & 0xFF);
            }
            else {
                buf.push_back((cp >> 24) & 0xFF);
                buf.push_back((cp >> 16) & 0xFF);
                buf.push_back((cp >> 8) & 0xFF);
                buf.push_back(cp & 0xFF);
            }
        }

        return buf;
    }

    static bool WStringToUtf8(const std::wstring& text, std::vector<BYTE>& out) {
        out.clear();
        if (text.empty()) return true;

        int needed = WideCharToMultiByte(
            CP_UTF8, 0, text.c_str(), (int)text.size(),
            nullptr, 0, nullptr, nullptr
        );
        if (needed <= 0) return false;

        out.resize((size_t)needed);
        WideCharToMultiByte(
            CP_UTF8, 0, text.c_str(), (int)text.size(),
            reinterpret_cast<char*>(out.data()), needed, nullptr, nullptr
        );
        return true;
    }

    static bool WStringToAnsi(const std::wstring& text, std::vector<BYTE>& out) {
        out.clear();
        if (text.empty()) return true;

        int needed = WideCharToMultiByte(
            CP_ACP, 0, text.c_str(), (int)text.size(),
            nullptr, 0, nullptr, nullptr
        );
        if (needed <= 0) return false;

        out.resize((size_t)needed);
        WideCharToMultiByte(
            CP_ACP, 0, text.c_str(), (int)text.size(),
            reinterpret_cast<char*>(out.data()), needed, nullptr, nullptr
        );
        return true;
    }

    bool BytesToText(
        const std::vector<BYTE>& bytes,
        Encoding encoding,
        std::wstring& outText
    ) {
        outText.clear();

        const size_t n = bytes.size();
        const BYTE* p = bytes.data();

        switch (encoding) {
        case Encoding::Utf16LE: {
            size_t offset = (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) ? 2 : 0;
            size_t count = (n - offset) / 2;
            if (count > 0) {
                outText.resize(count);
                memcpy(&outText[0], p + offset, count * sizeof(wchar_t));
            }
            return true;
        }

        case Encoding::Utf16BE: {
            size_t offset = (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) ? 2 : 0;
            size_t count = (n - offset) / 2;
            outText.resize(count);
            for (size_t i = 0; i < count; ++i) {
                outText[i] = static_cast<wchar_t>(
                    (static_cast<wchar_t>(p[offset + 2 * i]) << 8) |
                    static_cast<wchar_t>(p[offset + 2 * i + 1])
                    );
            }
            return true;
        }

        case Encoding::Utf32LE:
        case Encoding::Utf32BE: {
            bool le = (encoding == Encoding::Utf32LE);

            size_t offset = 0;
            if (le && n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0x00 && p[3] == 0x00)
                offset = 4;
            if (!le && n >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFE && p[3] == 0xFF)
                offset = 4;

            size_t count = (n - offset) / 4;
            outText.reserve(count);

            for (size_t i = 0; i < count; ++i) {
                const BYTE* u = p + offset + 4 * i;
                uint32_t cp;
                if (le) {
                    cp = u[0] | (u[1] << 8) | (u[2] << 16) | (static_cast<uint32_t>(u[3]) << 24);
                }
                else {
                    cp = (static_cast<uint32_t>(u[0]) << 24) | (u[1] << 16) | (u[2] << 8) | u[3];
                }
                AppendCodePoint(outText, cp);
            }
            return true;
        }

        case Encoding::Utf8:
        case Encoding::Utf8Bom: {
            size_t offset = 0;
            if (encoding == Encoding::Utf8Bom &&
                n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
                offset = 3;
            }

            int remaining = (int)(n - offset);
            if (remaining <= 0) return true;

            int needed = MultiByteToWideChar(
                CP_UTF8, 0,
                reinterpret_cast<const char*>(p + offset), remaining,
                nullptr, 0
            );
            if (needed <= 0) return false;

            outText.resize((size_t)needed);
            MultiByteToWideChar(
                CP_UTF8, 0,
                reinterpret_cast<const char*>(p + offset), remaining,
                &outText[0], needed
            );
            return true;
        }

        case Encoding::Ansi:
        default: {
            if (n == 0) return true;

            int needed = MultiByteToWideChar(
                CP_ACP, 0,
                reinterpret_cast<const char*>(p), (int)n,
                nullptr, 0
            );
            if (needed <= 0) return false;

            outText.resize((size_t)needed);
            MultiByteToWideChar(
                CP_ACP, 0,
                reinterpret_cast<const char*>(p), (int)n,
                &outText[0], needed
            );
            return true;
        }
        }
    }

    // Read file
    LoadResult Load(const std::wstring& path) {
        LoadResult result;

        HANDLE hFile = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile == INVALID_HANDLE_VALUE) {
            result.errorCode = GetLastError();
            return result;
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(hFile, &fileSize)) {
            result.errorCode = GetLastError();
            CloseHandle(hFile);
            return result;
        }

        if (fileSize.QuadPart > (LONGLONG)MAX_LOAD_SIZE) {
            result.errorCode = ERROR_FILE_TOO_LARGE;
            CloseHandle(hFile);
            return result;
        }

        std::vector<BYTE> bytes((size_t)fileSize.QuadPart);

        if (!bytes.empty()) {
            DWORD bytesRead = 0;
            if (!ReadFile(hFile, bytes.data(), (DWORD)bytes.size(), &bytesRead, nullptr)) {
                result.errorCode = GetLastError();
                CloseHandle(hFile);
                return result;
            }
        }

        CloseHandle(hFile);

        result.encoding = DetectEncoding(bytes);

        if (!BytesToText(bytes, result.encoding, result.text)) {
            result.errorCode = ERROR_NO_UNICODE_TRANSLATION;
            return result;
        }

        result.ok = true;
        return result;
    }

    // Writing
    static bool WriteAll(HANDLE hFile, const void* data, DWORD size, DWORD& err) {
        const BYTE* p = static_cast<const BYTE*>(data);
        DWORD written = 0;

        while (written < size) {
            DWORD chunk = 0;
            if (!WriteFile(hFile, p + written, size - written, &chunk, nullptr)) {
                err = GetLastError();
                return false;
            }
            written += chunk;
        }

        return true;
    }

    SaveResult Save(
        const std::wstring& path,
        const std::wstring& text,
        Encoding encoding
    ) {
        SaveResult result;

        HANDLE hFile = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile == INVALID_HANDLE_VALUE) {
            result.errorCode = GetLastError();
            return result;
        }

        DWORD err = 0;
        bool ok = true;
        std::vector<BYTE> buffer;

        switch (encoding) {
        case Encoding::Ansi: {
            ok = WStringToAnsi(text, buffer);
            if (ok && !buffer.empty())
                ok = WriteAll(hFile, buffer.data(), (DWORD)buffer.size(), err);
            break;
        }

        case Encoding::Utf8: {
            ok = WStringToUtf8(text, buffer);
            if (ok && !buffer.empty())
                ok = WriteAll(hFile, buffer.data(), (DWORD)buffer.size(), err);
            break;
        }

        case Encoding::Utf8Bom: {
            static const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
            ok = WriteAll(hFile, bom, 3, err);
            if (ok) {
                ok = WStringToUtf8(text, buffer);
                if (ok && !buffer.empty())
                    ok = WriteAll(hFile, buffer.data(), (DWORD)buffer.size(), err);
            }
            break;
        }

        case Encoding::Utf16LE: {
            static const BYTE bom[2] = { 0xFF, 0xFE };
            ok = WriteAll(hFile, bom, 2, err);
            if (ok && !text.empty())
                ok = WriteAll(hFile, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), err);
            break;
        }

        case Encoding::Utf16BE: {
            static const BYTE bom[2] = { 0xFE, 0xFF };
            ok = WriteAll(hFile, bom, 2, err);
            if (ok) {
                buffer.resize(text.size() * 2);
                for (size_t i = 0; i < text.size(); ++i) {
                    wchar_t w = text[i];
                    buffer[2 * i] = (BYTE)((w >> 8) & 0xFF);
                    buffer[2 * i + 1] = (BYTE)(w & 0xFF);
                }
                if (!buffer.empty())
                    ok = WriteAll(hFile, buffer.data(), (DWORD)buffer.size(), err);
            }
            break;
        }

        case Encoding::Utf32LE: {
            static const BYTE bom[4] = { 0xFF, 0xFE, 0x00, 0x00 };
            ok = WriteAll(hFile, bom, 4, err);
            if (ok) {
                buffer = WStringToUtf32(text, true);
                if (!buffer.empty())
                    ok = WriteAll(hFile, buffer.data(), (DWORD)buffer.size(), err);
            }
            break;
        }

        case Encoding::Utf32BE: {
            static const BYTE bom[4] = { 0x00, 0x00, 0xFE, 0xFF };
            ok = WriteAll(hFile, bom, 4, err);
            if (ok) {
                buffer = WStringToUtf32(text, false);
                if (!buffer.empty())
                    ok = WriteAll(hFile, buffer.data(), (DWORD)buffer.size(), err);
            }
            break;
        }

        default:
            ok = false;
            break;
        }

        CloseHandle(hFile);

        if (!ok && err == 0) {
            err = ERROR_NO_UNICODE_TRANSLATION;
        }

        result.ok = ok;
        result.errorCode = err;
        return result;
    }

    // Encoding display name for UI
    std::wstring EncodingToDisplayName(Encoding encoding) {
        switch (encoding) {
        case Encoding::Ansi:    return L"ANSI";
        case Encoding::Utf8:    return L"UTF-8";
        case Encoding::Utf8Bom: return L"UTF-8 (BOM)";
        case Encoding::Utf16LE: return L"UTF-16 (LE)";
        case Encoding::Utf16BE: return L"UTF-16 (BE)";
        case Encoding::Utf32LE: return L"UTF-32 (LE)";
        case Encoding::Utf32BE: return L"UTF-32 (BE)";
        }
        return L"ANSI";
    }

    // Is text file
    static std::wstring ToLowerCopy(std::wstring s) {
        for (wchar_t& ch : s) {
            if (ch >= L'A' && ch <= L'Z') {
                ch = ch - L'A' + L'a';
            }
        }
        return s;
    }

    bool IsTextFileByExtension(const std::wstring& path) {
        std::wstring name = path;

        size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            name = name.substr(slash + 1);
        }

        std::wstring lower = ToLowerCopy(name);

        // Known names without extension
        static const wchar_t* kSpecialNames[] = {
            L"hosts", L"license", L"readme", L"makefile",
            L"gitignore", L"gitattributes", L"gitmodules",
            L"dockerfile", L"vagrantfile", L"editorconfig",
        };

        for (const wchar_t* special : kSpecialNames) {
            if (lower == special) return true;
        }

        size_t dot = lower.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == lower.size() - 1) {
            return false;
        }

        std::wstring ext = lower.substr(dot + 1);

        static const wchar_t* kTextExtensions[] = {
            // Text / logs / configs
            L"txt", L"log", L"ini", L"cfg", L"conf", L"config", L"inf", L"manifest",
            L"xml", L"json", L"csv", L"tsv", L"md", L"yml", L"yaml", L"toml",
            // Windows / scrypts / registry
            L"reg", L"bat", L"cmd", L"ps1", L"psm1", L"psd1", L"vbs", L"js", L"wsf",
            // C / C++
            L"c", L"h", L"cpp", L"cxx", L"cc", L"hh", L"hpp", L"hxx", L"inl", L"ipp",
            L"rc", L"def", L"map", L"asm",
            // Other languages
            L"cs", L"java", L"py", L"rb", L"php", L"lua", L"pl", L"sh", L"sql",
            // Web
            L"html", L"htm", L"css", L"scss", L"less", L"svg",
            // VS / MSBuild
            L"props", L"targets", L"vcxproj", L"sln", L"csproj",
        };

        for (const wchar_t* e : kTextExtensions) {
            if (ext == e) return true;
        }

        return false;
    }

}