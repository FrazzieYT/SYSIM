#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace TextFile {

    //--------------------------------------------------
    // Поддерживаемые кодировки
    //--------------------------------------------------
    enum class Encoding {
        Ansi,       // системная кодовая страница (CP_ACP)
        Utf8,       // UTF-8 без BOM
        Utf8Bom,    // UTF-8 с BOM
        Utf16LE,    // UTF-16 Little Endian
        Utf16BE,    // UTF-16 Big Endian
        Utf32LE,    // UTF-32 Little Endian
        Utf32BE     // UTF-32 Big Endian
    };

    struct LoadResult {
        bool ok = false;
        Encoding encoding = Encoding::Ansi;
        std::wstring text;
        DWORD errorCode = 0;
    };

    struct SaveResult {
        bool ok = false;
        DWORD errorCode = 0;
    };

    // Detect encoding from raw bytes
    Encoding DetectEncoding(const std::vector<BYTE>& bytes);

    // Convert raw bytes to wstring using specified encoding
    bool BytesToText(
        const std::vector<BYTE>& bytes,
        Encoding encoding,
        std::wstring& outText
    );

    // Read file from disk with auto-detect encoding
    LoadResult Load(const std::wstring& path);

    // Сохранение текста на диск в указанной кодировке
    SaveResult Save(
        const std::wstring& path,
        const std::wstring& text,
        Encoding encoding
    );

    // Human-readable encoding name
    std::wstring EncodingToDisplayName(Encoding encoding);

    // Is text file by extension or name
    bool IsTextFileByExtension(const std::wstring& path);

}